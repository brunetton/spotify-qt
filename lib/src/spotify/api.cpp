#include "lib/spotify/api.hpp"
#include "lib/uri.hpp"

lib::spt::api::api(lib::settings &settings, const lib::http_client &http_client)
	: settings(settings),
	http(http_client)
{
}

void lib::spt::api::refresh(bool force)
{
	constexpr long s_in_hour = 60 * 60;

	if (!force
		&& lib::date_time::seconds_since_epoch() - settings.account.last_refresh < s_in_hour)
	{
		lib::log::debug("Last refresh was less than an hour ago, not refreshing access token");
		last_auth = settings.account.last_refresh;
		return;
	}

	// Make sure we have a refresh token
	auto refresh_token = settings.account.refresh_token;
	if (refresh_token.empty())
	{
		throw lib::spt::error("No refresh token", "token");
	}

	// Create form
	auto post_data = lib::fmt::format("grant_type=refresh_token&refresh_token={}",
		refresh_token);

	// Create request
	auto auth_header = lib::fmt::format("Basic {}",
		lib::base64::encode(lib::fmt::format("{}:{}",
			settings.account.client_id, settings.account.client_secret)));

	// Send request
	auto reply = request_refresh(post_data, auth_header);
	if (reply.empty())
	{
		throw lib::spt::error("No response", "token");
	}

	// Parse as JSON
	const auto json = nlohmann::json::parse(reply);

	// Check if error
	if (json.contains("error_description") || !json.contains("access_token"))
	{
		auto error = json.at("error_description").get<std::string>();
		throw lib::spt::error(error.empty()
			? "No access token" : error, "token");
	}

	// Save as access token
	last_auth = lib::date_time::seconds_since_epoch();
	settings.account.last_refresh = last_auth;
	settings.account.access_token = json.at("access_token").get<std::string>();
	settings.save();
}

auto lib::spt::api::auth_headers() -> lib::headers
{
	constexpr int secsInHour = 3600;

	// See when last refresh was
	auto last_refresh = lib::date_time::seconds_since_epoch() - last_auth;
	if (last_refresh >= secsInHour)
	{
		lib::log::debug("Access token probably expired, refreshing");
		try
		{
			refresh();
		}
		catch (const std::exception &e)
		{
			lib::log::error("Refresh failed: {}", e.what());
		}
	}

	return {
		{
			"Authorization",
			lib::fmt::format("Bearer {}", settings.account.access_token),
		},
	};
}

auto lib::spt::api::parse_json(const std::string &url, const std::string &data) -> nlohmann::json
{
	// No data, no response, no error
	if (data.empty())
	{
		return {};
	}

	auto json = nlohmann::json::parse(data);

	if (!lib::spt::error::is(json))
	{
		return json;
	}

	auto err = lib::spt::error::error_message(json);
	lib::log::error("{} failed: {}", url, err);
	throw lib::spt::error(err, url);
}

auto lib::spt::api::request_refresh(const std::string &post_data,
	const std::string &authorization) -> std::string
{
	return http.post("https://accounts.spotify.com/api/token", {
		{"Content-Type", "application/x-www-form-urlencoded"},
		{"Authorization", authorization},
	}, post_data);
}

auto lib::spt::api::error_message(const std::string &url, const std::string &data) -> std::string
{
	nlohmann::json json;
	try
	{
		if (!data.empty())
		{
			json = nlohmann::json::parse(data);
		}
	}
	catch (const std::exception &e)
	{
		lib::log::warn("{} failed: {}", url, e.what());
		return {};
	}

	if (json.is_null() || !json.is_object() || !json.contains("error"))
	{
		return {};
	}

	auto message = json.at("error").at("message").get<std::string>();
	if (!message.empty())
	{
		lib::log::error("{} failed: {}", url, message);
	}
	return message;
}

void lib::spt::api::select_device(const std::vector<lib::spt::device> &/*devices*/,
	lib::callback<lib::spt::device> &callback)
{
	callback(lib::spt::device());
}

auto lib::spt::api::to_uri(const std::string &type, const std::string &id) -> std::string
{
	return lib::strings::starts_with(id, "spotify:")
		? id
		: lib::fmt::format("spotify:{}:{}", type, id);
}

auto lib::spt::api::to_id(const std::string &id) -> std::string
{
	auto i = id.rfind(':');
	return i != std::string::npos
		? id.substr(i + 1)
		: id;
}

auto lib::spt::api::to_full_url(const std::string &relative_url) -> std::string
{
	return lib::fmt::format("https://api.spotify.com/v1/{}", relative_url);
}

auto lib::spt::api::follow_type_string(lib::follow_type type) -> std::string
{
	switch (type)
	{
		case lib::follow_type::artist:
			return "artist";

		case lib::follow_type::user:
			return "user";
	}

	return {};
}

void lib::spt::api::set_current_device(const std::string &id)
{
	settings.general.last_device = id;
	settings.save();
}

auto lib::spt::api::get_current_device() const -> const std::string &
{
	return settings.general.last_device;
}

auto lib::spt::api::get_device_url(const std::string &url,
	const lib::spt::device &device) -> std::string
{
	lib::uri uri(lib::strings::starts_with(url, "https://")
		? url
		: to_full_url(url));

	auto params = uri.get_search_params();
	auto device_id = params.find("device_id");
	if (device_id != params.end())
	{
		device_id->second = device.id;
	}
	else
	{
		params.insert({
			"device_id",
			device.id,
		});
	}

	uri.set_search_params(params);
	auto pathname = uri.pathname();
	return lib::strings::remove(pathname, "/v1/");
}

//region GET

void lib::spt::api::get(const std::string &url, lib::callback<nlohmann::json> &callback)
{
	http.get(to_full_url(url), auth_headers(),
		[url, callback](const std::string &response)
		{
			try
			{
				callback(response.empty()
					? nlohmann::json()
					: nlohmann::json::parse(response));
			}
			catch (const nlohmann::json::parse_error &e)
			{
				lib::log::error("{} failed to parse: {}", url, e.what());
				lib::log::debug("JSON: {}", response);
			}
			catch (const std::exception &e)
			{
				lib::log::error("{} failed: {}", url, e.what());
			}
		});
}

void lib::spt::api::get_items(const std::string &url, const std::string &key,
	lib::callback<nlohmann::json> &callback)
{
	constexpr size_t api_prefix_length = 27;

	auto api_url = lib::strings::starts_with(url, "https://api.spotify.com/v1/")
		? url.substr(api_prefix_length)
		: url;

	get(api_url, [this, key, callback](const nlohmann::json &json)
	{
		if (!key.empty() && !json.contains(key))
		{
			lib::log::error(R"(no such key "{}" in "{}")", key, json.dump());
		}

		const auto &content = key.empty() ? json : json.at(key);
		const auto &items = content.at("items");
		if (content.contains("next") && content.at("next").is_string())
		{
			const auto &next = content.at("next").get<std::string>();
			get_items(next, key, [items, callback](const nlohmann::json &next)
			{
				callback(lib::json::combine(items, next));
			});
			return;
		}
		callback(items);
	});
}

void lib::spt::api::get_items(const std::string &url, lib::callback<nlohmann::json> &callback)
{
	get_items(url, std::string(), callback);
}

//endregion

//region PUT

void lib::spt::api::put(const std::string &url, const nlohmann::json &body,
	lib::callback<std::string> &callback)
{
	auto header = auth_headers();
	header["Content-Type"] = "application/json";

	auto data = body.is_null()
		? std::string()
		: body.dump();

	http.put(to_full_url(url), data, header,
		[this, url, body, callback](const std::string &response)
		{
			auto error = error_message(url, response);

			const auto noDevice = lib::strings::contains(error, "No active device found");
			const auto invalidDevice = lib::strings::contains(error, "Device not found");

			if (noDevice || invalidDevice)
			{
				if (invalidDevice)
				{
					set_current_device(std::string());
				}

				devices([this, url, body, error, callback]
					(const std::vector<lib::spt::device> &devices)
				{
					if (devices.empty())
					{
						if (callback)
						{
							callback(error);
						}
					}
					else
					{
						this->select_device(devices, [this, url, body, callback, error]
							(const lib::spt::device &device)
						{
							if (device.id.empty())
							{
								callback(error);
								return;
							}

							this->set_device(device, [this, url, body, callback, device]
								(const std::string &status)
							{
								if (status.empty())
								{
									set_current_device(device.id);
									this->put(get_device_url(url, device), body, callback);
								}
							});
						});
					}
				});
			}
			else if (callback)
			{
				callback(error);
			}
		});
}

void lib::spt::api::put(const std::string &url, lib::callback<std::string> &callback)
{
	put(url, nlohmann::json(), callback);
}

//endregion

//region POST

void lib::spt::api::post(const std::string &url, lib::callback<std::string> &callback)
{
	auto headers = auth_headers();
	headers["Content-Type"] = "application/x-www-form-urlencoded";

	http.post(to_full_url(url), headers, [url, callback](const std::string &response)
	{
		callback(error_message(url, response));
	});
}

void lib::spt::api::post(const std::string &url, const nlohmann::json &json,
	lib::callback<nlohmann::json> &callback)
{
	auto headers = auth_headers();
	headers["Content-Type"] = "application/json";

	auto data = json.is_null()
		? std::string()
		: json.dump();

	http.post(to_full_url(url), data, headers,
		[url, callback](const std::string &response)
		{
			try
			{
				callback(response.empty()
					? nlohmann::json()
					: nlohmann::json::parse(response));
			}
			catch (const nlohmann::json::parse_error &e)
			{
				lib::log::error("{} failed to parse: {}", url, e.what());
				lib::log::debug("JSON: {}", response);
			}
			catch (const std::exception &e)
			{
				lib::log::error("{} failed: {}", url, e.what());
			}
		});
}


//endregion

//region DELETE

void lib::spt::api::del(const std::string &url, const nlohmann::json &json,
	lib::callback<std::string> &callback)
{
	auto headers = auth_headers();
	headers["Content-Type"] = "application/json";

	auto data = json.is_null()
		? std::string()
		: json.dump();

	http.del(to_full_url(url), data, headers,
		[url, callback](const std::string &response)
		{
			callback(error_message(url, response));
		});
}

void lib::spt::api::del(const std::string &url, lib::callback<std::string> &callback)
{
	del(url, nlohmann::json(), callback);
}

//endregion
