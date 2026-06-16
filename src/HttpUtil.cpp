#include "HttpUtil.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/ssl.h>
#include <stdexcept>

namespace Exchange {

std::string perform_https_request(
    const std::string& host,
    const std::string& port,
    boost::beast::http::verb method,
    const std::string& target
) {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_none);

    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve(host, port);

    ssl::stream<beast::tcp_stream> stream(ioc, ctx);

    if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
        throw beast::system_error{ec};
    }

    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::empty_body> req{method, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "BoostBeastClient");

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    if (res.result() != http::status::ok) {
        throw std::runtime_error("HTTP request failed with status: " + std::to_string(static_cast<int>(res.result())));
    }

    beast::error_code ec;
    stream.shutdown(ec);

    return res.body();
}

} // namespace Exchange
