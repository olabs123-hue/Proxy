#pragma once
// Minimal, strict HTTP/1.1 parsing for the edge proxy.
//
// Design goals: incremental (works on partial reads), bounded memory, and
// conservative about anything that could enable request smuggling.
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace edge {

struct Header {
  std::string name;   // original case preserved
  std::string value;  // surrounding whitespace trimmed
};

bool iequals(const std::string& a, const char* b);
std::string to_lower(std::string s);
const std::string* find_header(const std::vector<Header>& headers, const char* name);
const char* default_reason(int status);

struct Request {
  std::string method;
  std::string target;   // origin-form, e.g. "/a/b?x=1"
  std::string version;  // "HTTP/1.0" or "HTTP/1.1"
  std::vector<Header> headers;
  std::string host;
  bool keep_alive = true;
};

enum class ParseStatus { NeedMore, Done, Error };

struct RequestLimits {
  size_t max_header_bytes = 16 * 1024;
  size_t max_headers = 100;
};

struct RequestParseResult {
  ParseStatus status = ParseStatus::NeedMore;
  size_t consumed = 0;   // bytes used by the request head (when Done)
  int error_status = 0;  // HTTP status to answer with (when Error)
  std::string error_message;
  Request request;
};

// Parses one request head from [data, data+len). Request bodies are not
// supported: a request that declares one is rejected.
RequestParseResult parse_request(const char* data, size_t len, const RequestLimits& limits);

struct Response {
  int status = 0;
  std::string reason;
  std::vector<Header> headers;            // as sent by the origin (hop-by-hop included)
  std::string body;                       // fully decoded (chunked encoding removed)
  std::optional<size_t> declared_length;  // Content-Length the origin sent, if any
  double stored_at = 0;                   // set when stored in the cache
};

// Incremental parser for an origin response. Buffers the whole body (bounded)
// and decodes Content-Length, chunked, and close-delimited bodies.
class ResponseParser {
 public:
  ResponseParser(bool head_request, size_t max_body_bytes, size_t max_header_bytes = 32 * 1024);

  ParseStatus feed(const char* data, size_t len);
  // Call when the origin closed the connection.
  ParseStatus on_eof();

  Response& response() { return resp_; }
  bool too_large() const { return too_large_; }
  const std::string& error() const { return error_; }
  bool received_any() const { return received_any_; }

 private:
  enum class State { Headers, BodyLength, BodyChunkSize, BodyChunkData, BodyChunkCrlf, BodyTrailers, BodyClose, Done, Failed };
  ParseStatus advance();
  ParseStatus fail(const std::string& msg);
  ParseStatus parse_head(size_t head_end);

  bool head_request_;
  size_t max_body_;
  size_t max_header_;
  State state_ = State::Headers;
  std::string buf_;
  Response resp_;
  size_t remaining_ = 0;
  size_t trailer_bytes_ = 0;
  bool too_large_ = false;
  bool received_any_ = false;
  std::string error_;
};

// Hop-by-hop headers must not be forwarded by a proxy.
bool is_hop_by_hop(const std::string& lower_name);

struct CacheControl {
  bool no_store = false;
  bool no_cache = false;
  bool is_private = false;
  std::optional<long> max_age;
  std::optional<long> s_maxage;
};
CacheControl parse_cache_control(const std::vector<Header>& headers);

// Serializes the head of a response to a client. Always emits an explicit
// Content-Length and Connection header; strips hop-by-hop headers.
std::string serialize_response_head(const Response& r, bool is_head_request, bool keep_alive,
                                    const char* x_cache, long age_seconds);

// Builds the request sent to an origin (Connection: close, adds X-Forwarded-For).
std::string build_upstream_request(const Request& req, const std::string& client_ip);

}  // namespace edge
