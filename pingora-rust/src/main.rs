//! EdgeGuard: HTTP reverse proxy and load-balancer research prototype.
//! Deliberately loopback-only by default; not intended for production exposure.
use async_trait::async_trait;
use pingora::prelude::*;
use std::{env, sync::Arc, time::Duration};

struct EdgeGuard(Arc<LoadBalancer<RoundRobin>>);

#[async_trait]
impl ProxyHttp for EdgeGuard {
    type CTX = ();

    fn new_ctx(&self) -> Self::CTX {}

    async fn upstream_peer(&self, _session: &mut Session, _ctx: &mut Self::CTX) -> Result<Box<HttpPeer>> {
        // Do not panic or take the process down when all upstreams are unhealthy.
        let upstream = self.0.select(b"", 256).ok_or_else(|| {
            pingora::Error::explain(
                pingora::ErrorType::HTTPStatus(503),
                "no healthy upstreams available",
            )
        })?;
        Ok(Box::new(HttpPeer::new(upstream, false, String::new())))
    }
}

fn main() {
    let listen = env::var("EDGEGUARD_LISTEN")
        .unwrap_or_else(|_| "127.0.0.1:6188".to_owned());
    let backends = env::var("EDGEGUARD_BACKENDS")
        .unwrap_or_else(|_| "127.0.0.1:9001,127.0.0.1:9002".to_owned());
    let addresses: Vec<String> = backends.split(',')
        .map(str::trim)
        .filter(|v| !v.is_empty())
        .map(str::to_owned)
        .collect();
    assert!(!addresses.is_empty(), "EDGEGUARD_BACKENDS must not be empty");

    let mut server = Server::new(None).expect("create Pingora server");
    server.bootstrap();
    let mut upstreams = LoadBalancer::try_from_iter(addresses.iter().map(String::as_str))
        .expect("valid backend addresses");
    upstreams.set_health_check(TcpHealthCheck::new());
    upstreams.health_check_frequency = Some(Duration::from_secs(1));
    let health_checks = background_service("tcp health checks", upstreams);
    let mut proxy = http_proxy_service(
        &server.configuration, EdgeGuard(health_checks.task())
    );
    proxy.add_tcp(&listen);
    server.add_service(proxy);
    server.add_service(health_checks);
    println!("EdgeGuard listening on http://{listen}; origins: {backends}");
    server.run_forever();
}
