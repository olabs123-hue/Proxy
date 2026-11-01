#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

#include "http.h"

using namespace edge;

namespace {
RequestParseResult parse(const std::string& s, RequestLimits l = {}) { return parse_request(s.data(), s.size(), l); }
}  // namespace

TEST(RequestParser, ParsesSimpleGet) {
  auto r = parse("GET /a/b?x=1 HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n");
  ASSERT_EQ(r.status, ParseStatus::Done);
  EXPECT_EQ(r.request.method, "GET");
  EXPECT_EQ(r.request.target, "/a/b?x=1");
  EXPECT_EQ(r.request.host, "example.com");
  EXPECT_TRUE(r.request.keep_alive);
  ASSERT_NE(find_header(r.request.headers, "ACCEPT"), nullptr);  // case-insensitive lookup
  EXPECT_EQ(*find_header(r.request.headers, "accept"), "*/*");
}

TEST(RequestParser, ConsumesOnlyTheFirstRequest) {
  std::string two = "GET /1 HTTP/1.1\r\nHost: h\r\n\r\nGET /2 HTTP/1.1\r\nHost: h\r\n\r\n";
  auto r = parse(two);
  ASSERT_EQ(r.status, ParseStatus::Done);
  EXPECT_EQ(r.consumed, two.find("GET /2"));
}

TEST(RequestParser, PartialInputNeedsMore) {
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nHost: h\r\n").status, ParseStatus::NeedMore);
  EXPECT_EQ(parse("").status, ParseStatus::NeedMore);
}

TEST(RequestParser, OversizedHeadersAreRejected) {
  RequestLimits l;
  l.max_header_bytes = 64;
  auto r = parse("GET / HTTP/1.1\r\nHost: h\r\nX-Big: " + std::string(200, 'a'), l);
  EXPECT_EQ(r.status, ParseStatus::Error);
  EXPECT_EQ(r.error_status, 431);
}

TEST(RequestParser, ConnectionHandling) {
  auto close = parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
  EXPECT_FALSE(close.request.keep_alive);
  auto h10 = parse("GET / HTTP/1.0\r\n\r\n");  // Host is optional in HTTP/1.0
  ASSERT_EQ(h10.status, ParseStatus::Done);
  EXPECT_FALSE(h10.request.keep_alive);
  auto h10ka = parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
  EXPECT_TRUE(h10ka.request.keep_alive);
}

TEST(RequestParser, RejectsSmugglingVectors) {
  // CL + TE together (CL.TE / TE.CL)
  auto a = parse("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n");
  EXPECT_EQ(a.status, ParseStatus::Error);
  EXPECT_EQ(a.error_status, 400);
  // Conflicting duplicate Content-Length
  auto b = parse("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\nContent-Length: 5\r\n\r\n");
  EXPECT_EQ(b.error_status, 400);
  // Whitespace before the colon
  auto c = parse("GET / HTTP/1.1\r\nHost: h\r\nContent-Length : 0\r\n\r\n");
  EXPECT_EQ(c.error_status, 400);
  // Obsolete line folding
  auto d = parse("GET / HTTP/1.1\r\nHost: h\r\nX-A: b\r\n c\r\n\r\n");
  EXPECT_EQ(d.error_status, 400);
  // Bare LF inside a line
  auto e = parse("GET / HTTP/1.1\r\nHost: h\r\nX-A: b\nX-B: c\r\n\r\n");
  EXPECT_EQ(e.error_status, 400);
  // NUL byte
  std::string nul = "GET / HTTP/1.1\r\nHost: h\r\nX-A: b";
  nul.push_back('\0');
  nul += "c\r\n\r\n";
  EXPECT_EQ(parse(nul).error_status, 400);
  // Two Host headers
  auto f = parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
  EXPECT_EQ(f.error_status, 400);
}

TEST(RequestParser, RejectsUnsupportedFraming) {
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n").error_status, 501);
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello").error_status, 400);
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: abc\r\n\r\n").error_status, 400);
  // Content-Length: 0 is fine.
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n").status, ParseStatus::Done);
}

TEST(RequestParser, RejectsMalformedRequestLines) {
  EXPECT_EQ(parse("GET /\r\nHost: h\r\n\r\n").error_status, 400);                      // no version
  EXPECT_EQ(parse("GET / HTTP/1.1 extra\r\nHost: h\r\n\r\n").error_status, 400);       // extra token
  EXPECT_EQ(parse("GET http://x/ HTTP/1.1\r\nHost: h\r\n\r\n").error_status, 400);     // absolute-form
  EXPECT_EQ(parse("GET / HTTP/2.0\r\nHost: h\r\n\r\n").error_status, 505);
  EXPECT_EQ(parse("GET / HTTP/1.1\r\n\r\n").error_status, 400);                        // missing Host
  EXPECT_EQ(parse("G@T / HTTP/1.1\r\nHost: h\r\n\r\n").error_status, 400);             // bad method token
  EXPECT_EQ(parse("\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n").error_status, 400);         // leading blank line
  EXPECT_EQ(parse("GET /" + std::string(9000, 'a') + " HTTP/1.1\r\nHost: h\r\n\r\n", RequestLimits{65536, 100}).error_status, 414);
}

TEST(RequestParser, LimitsHeaderCount) {
  std::string req = "GET / HTTP/1.1\r\nHost: h\r\n";
  for (int i = 0; i < 150; ++i) req += "X-" + std::to_string(i) + ": v\r\n";
  req += "\r\n";
  EXPECT_EQ(parse(req).error_status, 431);
}

// ---------------------------------------------------------------------------

namespace {
ParseStatus feed_all(ResponseParser& p, const std::string& s, size_t step) {
  ParseStatus st = ParseStatus::NeedMore;
  for (size_t i = 0; i < s.size(); i += step) {
    st = p.feed(s.data() + i, std::min(step, s.size() - i));
    if (st != ParseStatus::NeedMore) return st;
  }
  return st;
}
}  // namespace

TEST(ResponseParser, ContentLengthBodyAcrossManySmallReads) {
  const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nX-A: b\r\n\r\nhello world";
  for (size_t step : {1u, 3u, 7u, 100u}) {
    ResponseParser p(false, 1024);
    ASSERT_EQ(feed_all(p, raw, step), ParseStatus::Done) << "step " << step;
    EXPECT_EQ(p.response().status, 200);
    EXPECT_EQ(p.response().body, "hello world");
    EXPECT_EQ(*find_header(p.response().headers, "x-a"), "b");
  }
}

TEST(ResponseParser, DecodesChunkedWithExtensionsAndTrailers) {
  const std::string raw =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nTrailer-X: y\r\n\r\n";
  for (size_t step : {1u, 2u, 5u, 1000u}) {
    ResponseParser p(false, 1024);
    ASSERT_EQ(feed_all(p, raw, step), ParseStatus::Done) << "step " << step;
    EXPECT_EQ(p.response().body, "hello world");
  }
}

TEST(ResponseParser, CloseDelimitedBodyEndsAtEof) {
  ResponseParser p(false, 1024);
  EXPECT_EQ(p.feed("HTTP/1.1 200 OK\r\n\r\nabc", 22), ParseStatus::NeedMore);
  EXPECT_EQ(p.feed("def", 3), ParseStatus::NeedMore);
  EXPECT_EQ(p.on_eof(), ParseStatus::Done);
  EXPECT_EQ(p.response().body, "abcdef");
}

TEST(ResponseParser, EofMidBodyIsAnError) {
  ResponseParser p(false, 1024);
  std::string partial = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
  p.feed(partial.data(), partial.size());
  EXPECT_EQ(p.on_eof(), ParseStatus::Error);
  ResponseParser q(false, 1024);
  EXPECT_EQ(q.on_eof(), ParseStatus::Error);  // closed without any response
  EXPECT_FALSE(q.received_any());
}

TEST(ResponseParser, BodylessResponses) {
  ResponseParser head(true, 1024);
  std::string h = "HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n";
  ASSERT_EQ(head.feed(h.data(), h.size()), ParseStatus::Done);
  EXPECT_TRUE(head.response().body.empty());
  EXPECT_EQ(*head.response().declared_length, 500u);
  ResponseParser nc(false, 1024);
  std::string n = "HTTP/1.1 204 No Content\r\n\r\n";
  EXPECT_EQ(nc.feed(n.data(), n.size()), ParseStatus::Done);
  ResponseParser nm(false, 1024);
  std::string m = "HTTP/1.1 304 Not Modified\r\nETag: x\r\n\r\n";
  EXPECT_EQ(nm.feed(m.data(), m.size()), ParseStatus::Done);
}

TEST(ResponseParser, EnforcesBodyLimit) {
  ResponseParser a(false, 10);
  std::string s = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
  EXPECT_EQ(a.feed(s.data(), s.size()), ParseStatus::Error);
  EXPECT_TRUE(a.too_large());
  ResponseParser b(false, 10);
  std::string c = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n20\r\n" + std::string(32, 'x') + "\r\n0\r\n\r\n";
  EXPECT_EQ(b.feed(c.data(), c.size()), ParseStatus::Error);
  EXPECT_TRUE(b.too_large());
  ResponseParser d(false, 10);
  std::string e = "HTTP/1.1 200 OK\r\n\r\n" + std::string(50, 'x');
  EXPECT_EQ(d.feed(e.data(), e.size()), ParseStatus::Error);
  EXPECT_TRUE(d.too_large());
}

TEST(ResponseParser, RejectsGarbage) {
  for (const char* bad : {"garbage\r\n\r\n", "HTTP/1.1 abc OK\r\n\r\n", "HTTP/1.1 200 OK\r\nno-colon\r\n\r\n",
                          "HTTP/1.1 100 Continue\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx",
                          "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
                          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n"}) {
    ResponseParser p(false, 1024);
    EXPECT_EQ(p.feed(bad, strlen(bad)), ParseStatus::Error) << bad;
  }
}

TEST(Serialization, ResponseHeadStripsHopByHopAndSetsLength) {
  Response r;
  r.status = 200;
  r.reason = "OK";
  r.headers = {{"Connection", "X-Secret, close"}, {"X-Secret", "1"}, {"Keep-Alive", "timeout=5"},
               {"Transfer-Encoding", "chunked"}, {"Content-Length", "999"}, {"X-Cache", "forged"},
               {"Age", "999"},                   {"X-Keep", "yes"}};
  r.body = "hello";
  std::string head = serialize_response_head(r, false, true, "HIT", 7);
  EXPECT_NE(head.find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_EQ(head.find("999"), std::string::npos);
  EXPECT_EQ(head.find("X-Secret"), std::string::npos);
  EXPECT_EQ(head.find("Keep-Alive: timeout"), std::string::npos);
  EXPECT_EQ(head.find("chunked"), std::string::npos);
  EXPECT_EQ(head.find("forged"), std::string::npos);
  EXPECT_NE(head.find("X-Keep: yes\r\n"), std::string::npos);
  EXPECT_NE(head.find("X-Cache: HIT\r\n"), std::string::npos);
  EXPECT_NE(head.find("Age: 7\r\n"), std::string::npos);
  EXPECT_NE(head.find("Connection: keep-alive\r\n"), std::string::npos);
  EXPECT_EQ(head.substr(head.size() - 4), "\r\n\r\n");
}

TEST(Serialization, HeadResponsesKeepDeclaredLengthWithoutBody) {
  Response r;
  r.status = 200;
  r.declared_length = 1234;
  std::string head = serialize_response_head(r, true, false, nullptr, -1);
  EXPECT_NE(head.find("Content-Length: 1234\r\n"), std::string::npos);
  EXPECT_NE(head.find("Connection: close\r\n"), std::string::npos);
}

TEST(Serialization, UpstreamRequestAppendsForwardedForAndStripsHopByHop) {
  auto parsed = parse(
      "GET /x?y=1 HTTP/1.1\r\nHost: shop.example\r\nConnection: keep-alive, X-Drop\r\nX-Drop: 1\r\n"
      "X-Forwarded-For: 1.1.1.1\r\nUser-Agent: t\r\nKeep-Alive: 5\r\n\r\n");
  ASSERT_EQ(parsed.status, ParseStatus::Done);
  std::string up = build_upstream_request(parsed.request, "9.9.9.9");
  EXPECT_EQ(up.rfind("GET /x?y=1 HTTP/1.1\r\nHost: shop.example\r\n", 0), 0u);
  EXPECT_NE(up.find("X-Forwarded-For: 1.1.1.1, 9.9.9.9\r\n"), std::string::npos);
  EXPECT_NE(up.find("User-Agent: t\r\n"), std::string::npos);
  EXPECT_EQ(up.find("X-Drop"), std::string::npos);
  EXPECT_EQ(up.find("Keep-Alive"), std::string::npos);
  EXPECT_NE(up.find("Connection: close\r\n\r\n"), std::string::npos);
}

TEST(CacheControlParsing, ParsesDirectives) {
  std::vector<Header> h = {{"Cache-Control", "Public, max-age=60, s-maxage=\"120\""}, {"Cache-Control", "no-store"}};
  CacheControl cc = parse_cache_control(h);
  EXPECT_TRUE(cc.no_store);
  EXPECT_FALSE(cc.is_private);
  ASSERT_TRUE(cc.max_age);
  EXPECT_EQ(*cc.max_age, 60);
  ASSERT_TRUE(cc.s_maxage);
  EXPECT_EQ(*cc.s_maxage, 120);
  std::vector<Header> bad = {{"Cache-Control", "max-age=abc, private"}};
  CacheControl b = parse_cache_control(bad);
  EXPECT_FALSE(b.max_age);
  EXPECT_TRUE(b.is_private);
}
