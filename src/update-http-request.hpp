#pragma once

#include <string>

struct update_client;
struct update_file_t;
 
using manifest_body = http::basic_dynamic_body<beast::flat_buffer>;

template <class Body, bool IncludeVersion>
struct update_http_request
{
	update_http_request(update_client *client_ctx, const std::string &target, const int id);
	~update_http_request();

	size_t download_accum{ 0 };
	size_t content_length{ 0 };
	int worker_id;
	update_client *client_ctx;
	std::string target;
	
	/* We used to support http and then I realized
	 * I was spending a lot of time supporting both.
	 * Our use case doesn't use it and boost doesn't
	 * allow any convenience to allow using them
	 * interchangeably. Really, my suggestion is that
	 * you should be using ssl regardless anyways. */
	ssl::stream<tcp::socket> ssl_socket;

	http::request<http::empty_body> request;

	beast::multi_buffer response_buf;
	http::response_parser<Body> response_parser;

	/* We need way to detect stuck connection.
	*  For that we use boost deadline timer what can limit
	*  time for each step of file downloader connection.
	*  Also it limits a recieve buffer so a timer limit a too slow fill of the buffer.
	*/
	boost::asio::deadline_timer deadline;
	int deadline_default_timeout = 5;
	bool deadline_reached = false;
	int retries = 0;

	void check_deadline_callback_err(const boost::system::error_code& error);
	void set_deadline();

	void handle_download_canceled();
	void handle_download_error(const boost::system::error_code & error, const char * str);
	void handle_result(update_file_t *file_ctx);

	void start_connect();
	void handle_connect(const boost::system::error_code &error, const tcp::endpoint &ep);
	void handle_handshake(const boost::system::error_code& error);
	void handle_request(boost::system::error_code &error, size_t bytes);
	void handle_response_header(boost::system::error_code &error, size_t bytes);
	void start_reading();
	void handle_response_body(boost::system::error_code &error, size_t bytes_read, update_file_t *file_ctx);
};


// limit response_buf buffer size so together with deadline timeout 
// it will create low speed limit for download 
// 4kb in 4 seconds is somewhere of old modems 
// and be recognized as stuck connection

template <class Body, bool IncludeVersion>
update_http_request<Body, IncludeVersion>::update_http_request(update_client *client_ctx, const std::string &target, const int id) :
	worker_id(id),
	client_ctx(client_ctx),
	target(target),
	ssl_socket(client_ctx->io_ctx, client_ctx->ssl_context),
	response_buf(file_buffer_size), // see reasons above ^
	deadline(client_ctx->io_ctx)
{
	std::string full_target;

	if (IncludeVersion)
	{
		full_target = fmt::format("{}/{}/{}", client_ctx->params->host.path, client_ctx->params->version, target);
	}
	else {
		full_target = fmt::format("{}/{}", client_ctx->params->host.path, target);
	}

	request = { http::verb::get, full_target, 11 };

	request.set(http::field::host, client_ctx->params->host.authority);

	request.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

	response_parser.body_limit(std::numeric_limits<unsigned long long>::max());

	deadline.expires_at(boost::posix_time::pos_infin);
}

template<class Body, bool IncludeVersion>
update_http_request<Body, IncludeVersion>::~update_http_request()
{
	deadline.cancel();
}

template<class Body, bool IncludeVersion>
void update_http_request<Body, IncludeVersion>::check_deadline_callback_err(const boost::system::error_code& error)
{
	if (error)
	{
		if (error == boost::asio::error::operation_aborted)
		{
			return;
		}
		else {
			log_error("File download operation deadline error %i", error.value());
			return;
		}
	}

	if (deadline.expires_at() <= boost::asio::deadline_timer::traits_type::now())
	{
		log_info("Timeout for file download operation trigered.");
		deadline_reached = true;

		boost::system::error_code ignored_ec;
		ssl_socket.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);

		deadline.expires_at(boost::posix_time::pos_infin);
	}
	else {
		// Put the actor back to sleep.
		deadline.async_wait(bind(&update_http_request<Body, IncludeVersion>::check_deadline_callback_err, this, std::placeholders::_1));
	}
}

template<class Body, bool IncludeVersion>
void update_http_request<Body, IncludeVersion>::set_deadline()
{
	deadline.expires_from_now(boost::posix_time::seconds(deadline_default_timeout));
	check_deadline_callback_err(make_error_code(boost::system::errc::success));
}

template<class Body, bool IncludeVersion>
void update_http_request<Body, IncludeVersion>::start_connect()
{
	auto connect_handler = [this](auto e, auto b) {
		this->handle_connect(e, b);
	};

	set_deadline();

	asio::async_connect(ssl_socket.lowest_layer(), client_ctx->endpoints, connect_handler);
}

template<class Body, bool IncludeVersion>
void update_http_request<Body, IncludeVersion>::handle_connect(const boost::system::error_code &error, const tcp::endpoint &ep)
{
	deadline.cancel();

	if (client_ctx->update_canceled)
	{
		handle_download_canceled();
		return;
	}

	if (error)
	{
		std::string msg = fmt::format("Failed to connect to host for: {}", target);

		handle_download_error(error, msg.c_str());
		return;
	}

	auto handshake_handler = [this](auto e) {
		this->handle_handshake(e);
	};

	set_deadline();

	ssl_socket.async_handshake(ssl::stream_base::handshake_type::client, handshake_handler);
}

template<class Body, bool IncludeVersion>
void update_http_request<Body, IncludeVersion>::handle_handshake(const boost::system::error_code& error)
{
	deadline.cancel();

	if (client_ctx->update_canceled)
	{
		handle_download_canceled();
		return;
	}

	if (error)
	{
		std::string msg = fmt::format("Failed request handshake for: {}", target);

		handle_download_error(error, msg.c_str());
		return;
	}

	auto request_handler = [this ](auto e, auto b) {
		this->handle_request(e, b);
	};

	set_deadline();

	http::async_write(ssl_socket, request, request_handler);
}

template<class Body, bool IncludeVersion>
void update_http_request<Body, IncludeVersion>::handle_request(boost::system::error_code &error, size_t bytes )
{
	deadline.cancel();

	if (client_ctx->update_canceled)
	{
		handle_download_canceled();
		return;
	}

	if (error)
	{
		std::string msg = fmt::format("Failed on request for: {}", target);

		handle_download_error(error, msg.c_str());
		return;
	}

	auto read_handler = [this](auto i, auto e) {
		this->handle_response_header(i, e);
	};

	set_deadline();

	http::async_read_header(ssl_socket, response_buf, response_parser, read_handler);
}

template<class Body, bool IncludeVersion>
void update_http_request<Body, IncludeVersion>::handle_response_header(boost::system::error_code &error, size_t bytes)
{
	deadline.cancel();

	if (client_ctx->update_canceled)
	{
		handle_download_canceled();
		return;
	}

	if (error)
	{
		std::string msg = fmt::format("Failed response header for: {}", target);

		handle_download_error(error, msg.c_str());
		return;
	}

	int status_code = response_parser.get().result_int();
	if (status_code != 200)
	{
		auto target_info = request.target();

		std::string output_str = fmt::format("Server send status Code: {} for: {}", status_code, fmt::string_view(target_info.data(), target_info.size()));

		handle_download_error(boost::asio::error::basic_errors::connection_aborted, output_str.c_str());
		return;
	}

	try {
		content_length = response_parser.content_length().value();
	}
	catch (...) {
		content_length = 0;
	}

	if (content_length == 0)
	{
		auto target_info = request.target();

		std::string output_str = fmt::format("Receive empty header for: {}", fmt::string_view(target_info.data(), target_info.size()));

		handle_download_error(boost::asio::error::basic_errors::connection_aborted, output_str.c_str());
		return;
	}

	start_reading();
}
