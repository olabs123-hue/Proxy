#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace edge {

namespace {

bool is_token_char(unsigned char c) {
  if (std::isalnum(c)) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

bool is_token(std::string_view s) {
  if (s.empty()) return false;
  for (unsigned char c : s)
    if (!is_token_char(c)) return false;
  return true;
}

std::string trim_ows(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
  return std::string(s.substr(b, e - b));
}

bool valid_field_value(std::string_view v) {
  for (unsigned char c : v) {
    if (c == '\t') continue;
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool all_digits(const std::string& s) {
  if (s.empty() || s.size() > 18) return false;
  for (unsigned char c : s)
    if (!std::isdigit(c)) return false;
  return true;
}

std::vector<std::string> split_tokens(const std::string& value) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= value.size()) {
    size_t comma = value.find(',', pos);
    std::string tok = trim_ows(std::string_view(value).substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
    if (!tok.empty()) out.push_back(to_lower(tok));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

// Names listed in any Connection header are hop-by-hop for this hop.
std::vector<std::string> connection_tokens(const std::vector<Header>& headers) {
  std::vector<std::string> out;
  for (const auto& h : headers) {
    if (iequals(h.name, "connection")) {
      auto toks = split_tokens(h.value);
      out.insert(out.end(), toks.begin(), toks.end());
    }
  }
  return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

RequestParseResult make_error(int status, const std::string& msg) {
  RequestParseResult r;
  r.status = ParseStatus::Error;
  r.error_status = status;
  r.error_message = msg;
  return r;
}

}  // namespace

bool iequals(const std::string& a, const char* b) {
  size_t n = std::char_traits<char>::length(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

std::string to_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const std::string* find_header(const std::vector<Header>& headers, const char* name) {
  for (const auto& h : headers)
    if (iequals(h.name, name)) return &h.value;
  return nullptr;
}

const char* default_reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Status";
  }
}

RequestParseResult parse_request(const char* data, size_t len, const RequestLimits& limits) {
  std::string_view sv(data, len);
  size_t end = sv.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    if (len > limits.max_header_bytes) return make_error(431, "request header too large");
    RequestParseResult r;
    r.status = ParseStatus::NeedMore;
    return r;
  }
  if (end + 4 > limits.max_header_bytes) return make_error(431, "request header too large");

  std::string_view head = sv.substr(0, end);
  if (head.empty()) return make_error(400, "empty request line");

  // Split into lines on CRLF; no line may contain a stray CR, LF or NUL.
  std::vector<std::string_view> lines;
  size_t pos = 0;
  while (true) {
    size_t e = head.find("\r\n", pos);
    std::string_view line = head.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
    for (char c : line)
      if (c == '\r' || c == '\n' || c == '\0') return make_error(400, "invalid character in header block");
    lines.push_back(line);
    if (e == std::string_view::npos) break;
    pos = e + 2;
  }

  // Request line: METHOD SP target SP HTTP/x.y
  std::string_view rl = lines[0];
  size_t s1 = rl.find(' ');
  size_t s2 = s1 == std::string_view::npos ? s1 : rl.find(' ', s1 + 1);
  if (s1 == std::string_view::npos || s2 == std::string_view::npos || rl.find(' ', s2 + 1) != std::string_view::npos)
    return make_error(400, "malformed request line");
  RequestParseResult res;
  Request& req = res.request;
  std::string_view method = rl.substr(0, s1);
  std::string_view target = rl.substr(s1 + 1, s2 - s1 - 1);
  std::string_view version = rl.substr(s2 + 1);
  if (!is_token(method) || method.size() > 16) return make_error(400, "malformed method");
  if (target.empty() || target[0] != '/') return make_error(400, "only origin-form request targets are supported");
  if (target.size() > 8192) return make_error(414, "request target too long");
  for (unsigned char c : target)
    if (c <= 0x20 || c == 0x7f) return make_error(400, "invalid character in request target");
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    if (version.substr(0, 5) == "HTTP/") return make_error(505, "unsupported HTTP version");
    return make_error(400, "malformed HTTP version");
  }
  req.method = std::string(method);
  req.target = std::string(target);
  req.version = std::string(version);

  // Header fields.
  if (lines.size() - 1 > limits.max_headers) return make_error(431, "too many header fields");
  for (size_t i = 1; i < lines.size(); ++i) {
    std::string_view line = lines[i];
    if (line.empty()) return make_error(400, "empty header line");
    if (line[0] == ' ' || line[0] == '\t') return make_error(400, "obsolete line folding is not supported");
    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return make_error(400, "malformed header field");
    std::string_view name = line.substr(0, colon);
    if (!is_token(name)) return make_error(400, "invalid header field name");  // also rejects "Name : v"
    std::string value = trim_ows(line.substr(colon + 1));
    if (!valid_field_value(value)) return make_error(400, "invalid header field value");
    req.headers.push_back({std::string(name), std::move(value)});
  }

  // Host.
  int host_count = 0;
  for (const auto& h : req.headers) {
    if (iequals(h.name, "host")) {
      ++host_count;
      req.host = h.value;
    }
  }
  if (host_count > 1) return make_error(400, "multiple Host headers");
  if (host_count == 0 && req.version == "HTTP/1.1") return make_error(400, "missing Host header");

  // Message framing. Anything ambiguous is rejected: this is the main defence
  // against request smuggling (CL.TE / TE.CL / duplicate Content-Length).
  bool has_te = find_header(req.headers, "transfer-encoding") != nullptr;
  std::vector<std::string> cl_values;
  for (const auto& h : req.headers)
    if (iequals(h.name, "content-length")) cl_values.push_back(h.value);
  if (has_te && !cl_values.empty()) return make_error(400, "both Content-Length and Transfer-Encoding present");
  if (has_te) return make_error(501, "Transfer-Encoding on requests is not supported");
  for (const auto& v : cl_values) {
    if (!all_digits(v)) return make_error(400, "invalid Content-Length");
    if (v != cl_values[0]) return make_error(400, "conflicting Content-Length headers");
  }
  if (!cl_values.empty() && std::strtoull(cl_values[0].c_str(), nullptr, 10) != 0)
    return make_error(400, "request bodies are not supported");

  // Connection semantics.
  req.keep_alive = (req.version == "HTTP/1.1");
  for (const auto& tok : connection_tokens(req.headers)) {
    if (tok == "close") req.keep_alive = false;
    else if (tok == "keep-alive" && req.version == "HTTP/1.0") req.keep_alive = true;
  }

  res.status = ParseStatus::Done;
  res.consumed = end + 4;
  return res;
}

bool is_hop_by_hop(const std::string& n) {
  return n == "connection" || n == "keep-alive" || n == "proxy-authenticate" || n == "proxy-authorization" ||
         n == "proxy-connection" || n == "te" || n == "trailer" || n == "transfer-encoding" || n == "upgrade";
}

CacheControl parse_cache_control(const std::vector<Header>& headers) {
  CacheControl cc;
  for (const auto& h : headers) {
    if (!iequals(h.name, "cache-control")) continue;
    for (const auto& tok : split_tokens(h.value)) {
      size_t eq = tok.find('=');
      std::string name = eq == std::string::npos ? tok : tok.substr(0, eq);
      std::string val = eq == std::string::npos ? "" : tok.substr(eq + 1);
      val.erase(std::remove(val.begin(), val.end(), '"'), val.end());
      if (name == "no-store") cc.no_store = true;
      else if (name == "no-cache") cc.no_cache = true;
      else if (name == "private") cc.is_private = true;
      else if (name == "max-age" && all_digits(val)) cc.max_age = std::strtol(val.c_str(), nullptr, 10);
      else if (name == "s-maxage" && all_digits(val)) cc.s_maxage = std::strtol(val.c_str(), nullptr, 10);
    }
  }
  return cc;
}

// ---------------------------------------------------------------------------
// ResponseParser
// ---------------------------------------------------------------------------

ResponseParser::ResponseParser(bool head_request, size_t max_body_bytes, size_t max_header_bytes)
    : head_request_(head_request), max_body_(max_body_bytes), max_header_(max_header_bytes) {}

ParseStatus ResponseParser::fail(const std::string& msg) {
  state_ = State::Failed;
  error_ = msg;
  return ParseStatus::Error;
}

ParseStatus ResponseParser::feed(const char* data, size_t len) {
  if (len > 0) received_any_ = true;
  if (state_ == State::Done) return ParseStatus::Done;
  if (state_ == State::Failed) return ParseStatus::Error;
  buf_.append(data, len);
  return advance();
}

ParseStatus ResponseParser::on_eof() {
  if (state_ == State::Done) return ParseStatus::Done;
  if (state_ == State::BodyClose) {
    state_ = State::Done;
    return ParseStatus::Done;
  }
  if (state_ == State::Failed) return ParseStatus::Error;
  return fail(received_any_ ? "origin closed the connection mid-response" : "origin closed the connection without responding");
}

ParseStatus ResponseParser::parse_head(size_t head_end) {
  std::string_view head(buf_.data(), head_end);
  size_t pos = 0;
  bool first = true;
  while (true) {
    size_t e = head.find("\r\n", pos);
    std::string_view line = head.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
    if (first) {
      first = false;
      // HTTP/1.x SSS reason
      if (line.size() < 12 || line.substr(0, 5) != "HTTP/" || line[8] != ' ') return fail("malformed status line");
      std::string code(line.substr(9, 3));
      if (!all_digits(code)) return fail("malformed status code");
      resp_.status = std::atoi(code.c_str());
      resp_.reason = line.size() > 13 ? std::string(line.substr(13)) : "";
    } else {
      size_t colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0) return fail("malformed response header");
      std::string_view name = line.substr(0, colon);
      if (!is_token(name)) return fail("invalid response header name");
      resp_.headers.push_back({std::string(name), trim_ows(line.substr(colon + 1))});
    }
    if (e == std::string_view::npos) break;
    pos = e + 2;
  }
  buf_.erase(0, head_end + 4);

  if (resp_.status < 200) return fail("unexpected informational response");

  // Work out how the body is delimited.
  const std::string* te = find_header(resp_.headers, "transfer-encoding");
  std::vector<std::string> cl_values;
  for (const auto& h : resp_.headers)
    if (iequals(h.name, "content-length")) cl_values.push_back(h.value);
  for (const auto& v : cl_values) {
    if (!all_digits(v) || v != cl_values[0]) return fail("invalid Content-Length from origin");
  }
  if (!cl_values.empty()) resp_.declared_length = std::strtoull(cl_values[0].c_str(), nullptr, 10);

  if (head_request_ || resp_.status == 204 || resp_.status == 304) {
    state_ = State::Done;
    return ParseStatus::Done;
  }
  if (te) {
    auto toks = split_tokens(*te);
    if (toks.empty() || toks.back() != "chunked") return fail("unsupported Transfer-Encoding from origin");
    state_ = State::BodyChunkSize;
  } else if (resp_.declared_length) {
    if (*resp_.declared_length > max_body_) {
      too_large_ = true;
      return fail("origin response too large");
    }
    remaining_ = *resp_.declared_length;
    state_ = remaining_ == 0 ? State::Done : State::BodyLength;
  } else {
    state_ = State::BodyClose;
  }
  return ParseStatus::NeedMore;
}

ParseStatus ResponseParser::advance() {
  for (;;) {
    switch (state_) {
      case State::Headers: {
        size_t e = buf_.find("\r\n\r\n");
        if (e == std::string::npos) {
          if (buf_.size() > max_header_) return fail("origin response header too large");
          return ParseStatus::NeedMore;
        }
        if (e + 4 > max_header_) return fail("origin response header too large");
        if (parse_head(e) == ParseStatus::Error) return ParseStatus::Error;
        break;
      }
      case State::Done:
        return ParseStatus::Done;
      case State::Failed:
        return ParseStatus::Error;
      case State::BodyLength: {
        size_t take = std::min(remaining_, buf_.size());
        resp_.body.append(buf_, 0, take);
        remaining_ -= take;
        buf_.clear();
        if (remaining_ == 0) {
          state_ = State::Done;
          break;
        }
        return ParseStatus::NeedMore;
      }
      case State::BodyClose: {
        resp_.body.append(buf_);
        buf_.clear();
        if (resp_.body.size() > max_body_) {
          too_large_ = true;
          return fail("origin response too large");
        }
        return ParseStatus::NeedMore;
      }
      case State::BodyChunkSize: {
        size_t e = buf_.find("\r\n");
        if (e == std::string::npos) {
          if (buf_.size() > 128) return fail("chunk size line too long");
          return ParseStatus::NeedMore;
        }
        std::string line = buf_.substr(0, e);
        size_t semi = line.find(';');
        if (semi != std::string::npos) line.resize(semi);
        line = trim_ows(line);
        if (line.empty() || line.size() > 15) return fail("invalid chunk size");
        for (unsigned char c : line)
          if (!std::isxdigit(c)) return fail("invalid chunk size");
        size_t size = std::strtoull(line.c_str(), nullptr, 16);
        buf_.erase(0, e + 2);
        if (size == 0) {
          state_ = State::BodyTrailers;
        } else {
          if (resp_.body.size() + size > max_body_) {
            too_large_ = true;
            return fail("origin response too large");
          }
          remaining_ = size;
          state_ = State::BodyChunkData;
        }
        break;
      }
      case State::BodyChunkData: {
        size_t take = std::min(remaining_, buf_.size());
        resp_.body.append(buf_, 0, take);
        buf_.erase(0, take);
        remaining_ -= take;
        if (remaining_ == 0) {
          state_ = State::BodyChunkCrlf;
          break;
        }
        return ParseStatus::NeedMore;
      }
      case State::BodyChunkCrlf: {
        if (buf_.size() < 2) return ParseStatus::NeedMore;
        if (buf_[0] != '\r' || buf_[1] != '\n') return fail("missing CRLF after chunk data");
        buf_.erase(0, 2);
        state_ = State::BodyChunkSize;
        break;
      }
      case State::BodyTrailers: {
        size_t e = buf_.find("\r\n");
        if (e == std::string::npos) {
          if (buf_.size() > 8192) return fail("trailers too large");
          return ParseStatus::NeedMore;
        }
        if (e == 0) {
          buf_.erase(0, 2);
          state_ = State::Done;
          break;
        }
        trailer_bytes_ += e + 2;
        if (trailer_bytes_ > 8192) return fail("trailers too large");
        buf_.erase(0, e + 2);
        break;
      }
    }
  }
}

std::string serialize_response_head(const Response& r, bool is_head_request, bool keep_alive,
                                    const char* x_cache, long age_seconds) {
  std::string out;
  out.reserve(256);
  out += "HTTP/1.1 ";
  out += std::to_string(r.status);
  out += ' ';
  out += r.reason.empty() ? default_reason(r.status) : r.reason;
  out += "\r\n";
  auto conn_tokens = connection_tokens(r.headers);
  for (const auto& h : r.headers) {
    std::string lname = to_lower(h.name);
    if (is_hop_by_hop(lname) || lname == "content-length" || lname == "x-cache" || lname == "age") continue;
    if (contains(conn_tokens, lname)) continue;
    out += h.name;
    out += ": ";
    out += h.value;
    out += "\r\n";
  }
  bool no_body_status = r.status == 204 || r.status == 304 || r.status < 200;
  if (!no_body_status) {
    if (is_head_request) {
      if (r.declared_length) {
        out += "Content-Length: " + std::to_string(*r.declared_length) + "\r\n";
      }
    } else {
      out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    }
  }
  if (x_cache) {
    out += "X-Cache: ";
    out += x_cache;
    out += "\r\n";
  }
  if (age_seconds >= 0) out += "Age: " + std::to_string(age_seconds) + "\r\n";
  out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  out += "\r\n";
  return out;
}

std::string build_upstream_request(const Request& req, const std::string& client_ip) {
  std::string out;
  out.reserve(256);
  out += req.method;
  out += ' ';
  out += req.target;
  out += " HTTP/1.1\r\nHost: ";
  out += req.host;
  out += "\r\n";
  auto conn_tokens = connection_tokens(req.headers);
  std::string xff;
  for (const auto& h : req.headers) {
    std::string lname = to_lower(h.name);
    if (lname == "x-forwarded-for") {
      if (!xff.empty()) xff += ", ";
      xff += h.value;
      continue;
    }
    if (is_hop_by_hop(lname) || lname == "host" || lname == "content-length") continue;
    if (contains(conn_tokens, lname)) continue;
    out += h.name;
    out += ": ";
    out += h.value;
    out += "\r\n";
  }
  if (!xff.empty()) xff += ", ";
  xff += client_ip;
  out += "X-Forwarded-For: " + xff + "\r\n";
  out += "Connection: close\r\n\r\n";
  return out;
}

}  // namespace edge
