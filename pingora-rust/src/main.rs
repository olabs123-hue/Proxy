//! EdgeGuard: local-only Pingora load balancer with bounded connection retries and metrics.
use async_trait::async_trait;
use pingora::prelude::*;
use std::{
    env,
    io::{Read, Write},
    net::TcpListener,
    sync::{atomic::{AtomicU64, Ordering}, Arc},
    thread,
    time::{Duration, Instant},
};

const MAX_ATTEMPTS: u8 = 2;

#[derive(Default)]
struct Metrics {
    requests: AtomicU64,
    failures: AtomicU64,
    connection_retries: AtomicU64,
    latency_sum_us: AtomicU64,
    latency_le_5ms: AtomicU64,
    latency_le_25ms: AtomicU64,
    latency_le_100ms: AtomicU64,
    latency_le_500ms: AtomicU64,
}

impl Metrics {
    fn render(&self) -> String {
        let r = |value: &AtomicU64| value.load(Ordering::Relaxed);
        let total = r(&self.requests);
        format!(
            "# HELP edgeguard_requests_total Completed proxy requests\n             # TYPE edgeguard_requests_total counter\n             edgeguard_requests_total {total}\n             # HELP edgeguard_failures_total Requests with a terminal proxy error\n             # TYPE edgeguard_failures_total counter\n             edgeguard_failures_total {}\n             # HELP edgeguard_connection_retries_total Eligible upstream connection retries\n             # TYPE edgeguard_connection_retries_total counter\n             edgeguard_connection_retries_total {}\n             # HELP edgeguard_request_duration_seconds Approximate elapsed time for finished requests\n             # TYPE edgeguard_request_duration_seconds histogram\n             edgeguard_request_duration_seconds_bucket{{le=\"0.005\"}} {}\n             edgeguard_request_duration_seconds_bucket{{le=\"0.025\"}} {}\n             edgeguard_request_duration_seconds_bucket{{le=\"0.1\"}} {}\n             edgeguard_request_duration_seconds_bucket{{le=\"0.5\"}} {}\n             edgeguard_request_duration_seconds_bucket{{le=\"+Inf\"}} {total}\n             edgeguard_request_duration_seconds_sum {:.6}\n             edgeguard_request_duration_seconds_count {total}\n",
            r(&self.failures), r(&self.connection_retries),
            r(&self.latency_le_5ms), r(&self.latency_le_25ms),
            r(&self.latency_le_100ms), r(&self.latency_le_500ms),
            r(&self.latency_sum_us) as f64 / 1_000_000.0,
        )
    }
}

fn serve_metrics(addr: String, metrics: Arc<Metrics>) -> std::io::Result<()> {
    let listener = TcpListener::bind(&addr)?;
    println!("EdgeGuard metrics: http://{addr}/metrics");
    thread::spawn(move || {
        for stream in listener.incoming() {
            let Ok(mut stream) = stream else { continue };
            let _ = stream.set_read_timeout(Some(Duration::from_secs(2)));
            let _ = stream.set_write_timeout(Some(Duration::from_secs(2)));
            let mut buf = [0u8; 2048];
            let Ok(n) = stream.read(&mut buf) else { continue };
            let request_line = String::from_utf8_lossy(&buf[..n]);
            let ok = request_line.starts_with("GET /metrics ")
                || request_line.starts_with("GET /metrics?");
            let (status, payload) = if ok {
                ("200 OK", metrics.render())
            } else {
                ("404 Not Found", "not found\n".to_string())
            };
            let response = format!(
                "HTTP/1.1 {status}\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{payload}",
                payload.len()
            );
            let _ = stream.write_all(response.as_bytes());
        }
    });
    Ok(())
}

struct RequestCtx {
    started: Instant,
    attempts: u8,
    previous: Option<String>,
}

struct EdgeGuard {
    balancer: Arc<LoadBalancer<RoundRobin>>,
    metrics: Arc<Metrics>,
}

#[async_trait]
impl ProxyHttp for EdgeGuard {
    type CTX = RequestCtx;

    fn new_ctx(&self) -> Self::CTX {
        RequestCtx { started: Instant::now(), attempts: 0, previous: None }
    }

    async fn upstream_peer(&self, _session: &mut Session, ctx: &mut Self::CTX) -> Result<Box<HttpPeer>> {
        // Avoid retrying a failed peer; for two healthy peers this selects the other peer.
        // A single live peer after a failure cannot satisfy the distinct-backend condition.
        for _ in 0..256 {
            let backend = self.balancer.select(b"", 256).ok_or_else(|| {
                pingora::Error::explain(
                    pingora::ErrorType::HTTPStatus(503),
                    "no healthy upstreams available",
                )
            })?;
            let identifier = format!("{:?}", backend.addr);
            if ctx.previous.as_ref() == Some(&identifier) {
                continue;
            }
            ctx.previous = Some(identifier);
            ctx.attempts += 1;
            return Ok(Box::new(HttpPeer::new(backend, false, String::new())));
        }
        Err(pingora::Error::explain(
            pingora::ErrorType::HTTPStatus(502),
            "no distinct healthy upstream for retry",
        ))
    }

    fn fail_to_connect(
        &self,
        session: &mut Session,
        _peer: &HttpPeer,
        ctx: &mut Self::CTX,
        mut e: Box<pingora::Error>,
    ) -> Box<pingora::Error> {
        let method = session.req_header().method.as_str();
        let eligible = matches!(method, "GET" | "HEAD") && ctx.attempts < MAX_ATTEMPTS;
        if eligible {
            self.metrics.connection_retries.fetch_add(1, Ordering::Relaxed);
        }
        e.set_retry(eligible);
        e
    }

    async fn logging(&self, _session: &mut Session, error: Option<&pingora::Error>, ctx: &mut Self::CTX) {
        let duration_us = ctx.started.elapsed().as_micros().min(u64::MAX as u128) as u64;
        self.metrics.requests.fetch_add(1, Ordering::Relaxed);
        if error.is_some() {
            self.metrics.failures.fetch_add(1, Ordering::Relaxed);
        }
        self.metrics.latency_sum_us.fetch_add(duration_us, Ordering::Relaxed);
        if duration_us <= 5_000 { self.metrics.latency_le_5ms.fetch_add(1, Ordering::Relaxed); }
        if duration_us <= 25_000 { self.metrics.latency_le_25ms.fetch_add(1, Ordering::Relaxed); }
        if duration_us <= 100_000 { self.metrics.latency_le_100ms.fetch_add(1, Ordering::Relaxed); }
        if duration_us <= 500_000 { self.metrics.latency_le_500ms.fetch_add(1, Ordering::Relaxed); }
    }
}

fn main() {
    let listen = env::var("EDGEGUARD_LISTEN")
        .unwrap_or_else(|_| "127.0.0.1:6188".to_owned());
    let metrics_addr = env::var("EDGEGUARD_METRICS_LISTEN")
        .unwrap_or_else(|_| "127.0.0.1:9188".to_owned());
    let backends = env::var("EDGEGUARD_BACKENDS")
        .unwrap_or_else(|_| "127.0.0.1:9001,127.0.0.1:9002".to_owned());
    let addresses: Vec<String> = backends.split(',')
        .map(str::trim).filter(|v| !v.is_empty())
        .map(str::to_owned).collect();
    assert!(!addresses.is_empty(), "EDGEGUARD_BACKENDS must not be empty");

    let metrics = Arc::new(Metrics::default());
    serve_metrics(metrics_addr, Arc::clone(&metrics)).expect("bind loopback metrics listener");

    let mut server = Server::new(None).expect("create Pingora server");
    server.bootstrap();
    let mut upstreams = LoadBalancer::try_from_iter(addresses.iter().map(String::as_str))
        .expect("valid backend addresses");
    upstreams.set_health_check(TcpHealthCheck::new());
    upstreams.health_check_frequency = Some(Duration::from_secs(1));
    let health_checks = background_service("tcp health checks", upstreams);
    let mut proxy = http_proxy_service(
        &server.configuration,
        EdgeGuard { balancer: health_checks.task(), metrics },
    );
    proxy.add_tcp(&listen);
    server.add_service(proxy);
    server.add_service(health_checks);
    println!("EdgeGuard listening on http://{listen}; origins: {backends}");
    server.run_forever();
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn metrics_start_at_zero() {
        let m = Metrics::default();
        let output = m.render();
        assert!(output.contains("edgeguard_requests_total 0"));
        assert!(output.contains("edgeguard_request_duration_seconds_count 0"));
    }
    #[test]
    fn metrics_count_and_buckets() {
        let m = Metrics::default();
        m.requests.store(2, Ordering::Relaxed);
        m.failures.store(1, Ordering::Relaxed);
        m.latency_le_100ms.store(2, Ordering::Relaxed);
        assert!(m.render().contains("edgeguard_failures_total 1"));
        assert!(m.render().contains("edgeguard_request_duration_seconds_bucket{le=\"0.1\"} 2"));
    }
}
