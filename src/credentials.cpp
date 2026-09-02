#include "credentials.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "atomic_file.h"
#include "json_io.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <aclapi.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace
{

const char *const type_api_key = "api_key";
const char *const type_oauth = "oauth";

// character offsets into a utf-8 string. masking slices by character because a
// byte slice can cut a multi-byte sequence in half, and the result is printed,
// logged, and sometimes serialised as JSON - all of which require valid utf-8.
std::vector<std::size_t> utf8_offsets(const std::string &text)
{
	std::vector<std::size_t> offsets;
	for (std::size_t at = 0; at < text.size(); ++at)
	{
		if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80)
			offsets.push_back(at);
	}
	offsets.push_back(text.size());
	return offsets;
}

std::string environment_value(const std::string &name)
{
#ifdef _WIN32
	char *value = nullptr;
	std::size_t size = 0;
	if (_dupenv_s(&value, &size, name.c_str()) != 0 || value == nullptr)
		return {};
	std::string result(value);
	std::free(value);
	return result;
#else
	const char *value = std::getenv(name.c_str());
	return value == nullptr ? std::string{} : std::string(value);
#endif
}

std::string trimmed(const std::string &text)
{
	const std::size_t first = text.find_first_not_of(" \t\n\r\f\v");
	if (first == std::string::npos)
		return {};
	const std::size_t last = text.find_last_not_of(" \t\n\r\f\v");
	return text.substr(first, last - first + 1);
}

// "2026-09-02T07:42:51+00:00" - seconds precision, explicit UTC offset rather
// than a Z suffix, matching what already exists in stores on disk.
std::string utc_now_iso8601()
{
	const std::time_t now = std::time(nullptr);
	std::tm parts{};
#ifdef _WIN32
	gmtime_s(&parts, &now);
#else
	gmtime_r(&now, &parts);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+00:00", &parts);
	return buffer;
}

double utc_now_epoch()
{
	using clock = std::chrono::system_clock;
	return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// Restrict the store to its owner.
//
// On POSIX this is chmod 0600. On Windows it replaces the inherited ACL with a
// single entry granting the current user full control - the Python
// implementation returned early here, leaving the store readable by anything
// that could read the user profile.
void harden(const fs::path &path)
{
#ifdef _WIN32
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return;

	DWORD needed = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
	std::vector<unsigned char> buffer(needed);
	if (needed == 0 || !GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed))
	{
		CloseHandle(token);
		return;
	}
	CloseHandle(token);

	PSID user = reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;

	EXPLICIT_ACCESS_W access = {};
	access.grfAccessPermissions = GENERIC_ALL;
	access.grfAccessMode = SET_ACCESS;
	access.grfInheritance = NO_INHERITANCE;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_USER;
	access.Trustee.ptstrName = static_cast<LPWSTR>(user);

	PACL acl = nullptr;
	if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS)
		return;

	std::wstring wide = path.wstring();
	// PROTECTED_DACL_SECURITY_INFORMATION is the part that matters: without it
	// the inherited profile ACE stays in place and the new entry merely adds to
	// it.
	SetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT,
						  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
						  nullptr, nullptr, acl, nullptr);
	LocalFree(acl);
#else
	::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

bool store_load(const credential_store &store, json &out_data, odin_error &out_error)
{
	const fs::path path = credential_store_path(store);
	if (!fs::exists(path))
	{
		out_data = json{{"version", credential_store_version}, {"credentials", json::object()}};
		return true;
	}

	odin_error read_error;
	json data = json_read(path, read_error);
	if (failed(read_error))
	{
		fail(out_error, error_kind::config,
			 "credential store is unreadable: " + read_error.message);
		return false;
	}

	const auto credentials = data.find("credentials");
	if (credentials == data.end() || !credentials->is_object())
	{
		fail(out_error, error_kind::config,
			 "credential store is malformed: 'credentials' is not an object");
		return false;
	}

	out_data = std::move(data);
	return true;
}

bool store_save(const credential_store &store, const json &data, odin_error &out_error)
{
	const fs::path path = credential_store_path(store);
	json_write_atomic(path, data, out_error);
	if (failed(out_error))
		return false;
	harden(path);
	return true;
}

// borrowed, or nullptr when absent
const json *entry_for(const json &data, const std::string &name)
{
	const json &credentials = data.at("credentials");
	const auto found = credentials.find(name);
	return found == credentials.end() || !found->is_object() ? nullptr : &*found;
}

std::string string_field(const json &entry, const char *key)
{
	const auto found = entry.find(key);
	return found != entry.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

credential_expiry expiry_of(const json &entry)
{
	const auto expires = entry.find("expires");
	if (expires == entry.end() || !expires->is_number())
		return credential_expiry::unknown;
	return utc_now_epoch() >= expires->get<double>() ? credential_expiry::expired
													 : credential_expiry::valid;
}

} // namespace

void credential_store_configure(credential_store &store, const fs::path &root)
{
	store.root = root;
}

fs::path credential_store_path(const credential_store &store)
{
	return store.root / ".odin" / "credentials.json";
}

std::string credential_mask(const std::string &value)
{
	if (value.empty())
		return "<empty>";

	const std::vector<std::size_t> offsets = utf8_offsets(value);
	const std::size_t length = offsets.size() - 1;

	// a short secret is replaced entirely: showing three of eight characters
	// would give away most of it. the length is still visible, which is
	// accepted.
	if (length <= 12)
		return std::string(length, '*');

	return value.substr(0, offsets[3]) + "..." + value.substr(offsets[length - 4]);
}

std::string credential_redact(const std::string &text, const std::string &secret)
{
	if (secret.empty())
		return text;

	// three renderings of the same secret. a value that reaches a log after
	// being JSON-encoded appears in one of the escaped forms, not the raw one.
	std::vector<std::string> forms;
	forms.push_back(secret);

	const std::string ascii = json(secret).dump(-1, ' ', true, json::error_handler_t::replace);
	if (ascii.size() >= 2)
		forms.push_back(ascii.substr(1, ascii.size() - 2));

	const std::string minimal = json(secret).dump(-1, ' ', false, json::error_handler_t::replace);
	if (minimal.size() >= 2)
		forms.push_back(minimal.substr(1, minimal.size() - 2));

	// longest first, so a form that contains a shorter one is replaced whole
	// rather than being left as a partially-redacted fragment
	std::sort(forms.begin(), forms.end(),
			  [](const std::string &left, const std::string &right) {
				  return left.size() > right.size();
			  });
	forms.erase(std::unique(forms.begin(), forms.end()), forms.end());

	std::string result = text;
	for (const std::string &form : forms)
	{
		if (form.empty())
			continue;
		std::size_t at = 0;
		while ((at = result.find(form, at)) != std::string::npos)
		{
			result.replace(at, form.size(), "[REDACTED]");
			at += 10;
		}
	}
	return result;
}

std::vector<std::string> credential_names(const credential_store &store, odin_error &out_error)
{
	std::vector<std::string> names;
	json data;
	if (!store_load(store, data, out_error))
		return names;
	// nlohmann objects iterate in ascending key order, which is the sort the
	// Python implementation applied explicitly
	for (const auto &[name, value] : data.at("credentials").items())
		names.push_back(name);
	return names;
}

bool credential_describe(const credential_store &store, const std::string &name,
						 json &out_described, odin_error &out_error)
{
	json data;
	if (!store_load(store, data, out_error))
		return false;

	const json *entry = entry_for(data, name);
	if (entry == nullptr)
		return false;

	const std::string type = string_field(*entry, "type");

	out_described = json::object();
	out_described["name"] = name;
	out_described["type"] = type.empty() ? type_api_key : type;
	out_described["created"] = entry->value("created", json(nullptr));
	out_described["note"] = entry->value("note", json(nullptr));

	if (type == type_oauth)
	{
		out_described["value"] = credential_mask(string_field(*entry, "access"));
		out_described["expires"] = entry->value("expires", json(nullptr));
		switch (expiry_of(*entry))
		{
		case credential_expiry::expired: out_described["expired"] = true; break;
		case credential_expiry::valid: out_described["expired"] = false; break;
		case credential_expiry::unknown: out_described["expired"] = nullptr; break;
		}
	}
	else
	{
		out_described["value"] = credential_mask(string_field(*entry, "value"));
	}
	return true;
}

json credential_describe_all(const credential_store &store, odin_error &out_error)
{
	json all = json::array();
	for (const std::string &name : credential_names(store, out_error))
	{
		if (failed(out_error))
			return json::array();
		json described;
		if (credential_describe(store, name, described, out_error))
			all.push_back(std::move(described));
	}
	return all;
}

credential_expiry credential_is_expired(const credential_store &store, const std::string &name,
										odin_error &out_error)
{
	json data;
	if (!store_load(store, data, out_error))
		return credential_expiry::unknown;
	const json *entry = entry_for(data, name);
	return entry == nullptr ? credential_expiry::unknown : expiry_of(*entry);
}

bool credential_secret(const credential_store &store, const std::string &name,
					   std::string &out_secret, odin_error &out_error)
{
	json data;
	if (!store_load(store, data, out_error))
		return false;

	const json *entry = entry_for(data, name);
	if (entry == nullptr)
	{
		fail(out_error, error_kind::config,
			 "no credential named '" + name + "'. Add one with: odin auth set " + name);
		return false;
	}

	if (string_field(*entry, "type") == type_oauth)
	{
		const std::string access = string_field(*entry, "access");
		if (access.empty())
		{
			fail(out_error, error_kind::config, "credential '" + name + "' has no access token");
			return false;
		}
		// an expired token is refused here rather than sent and rejected by the
		// provider, so the message names the fix instead of surfacing a 401
		if (expiry_of(*entry) == credential_expiry::expired)
		{
			fail(out_error, error_kind::config,
				 "credential '" + name +
				   "' has expired; refresh it in the issuing tool and re-import, or run: "
				   "odin auth set " +
				   name);
			return false;
		}
		out_secret = access;
		return true;
	}

	const std::string value = string_field(*entry, "value");
	if (value.empty())
	{
		fail(out_error, error_kind::config, "credential '" + name + "' has no value");
		return false;
	}
	out_secret = value;
	return true;
}

bool credential_set_api_key(credential_store &store, const std::string &name,
							const std::string &value, const json &note, odin_error &out_error)
{
	const std::string cleaned = trimmed(value);
	if (cleaned.empty())
	{
		fail(out_error, error_kind::config, "refusing to store an empty credential");
		return false;
	}

	json data;
	if (!store_load(store, data, out_error))
		return false;

	data["credentials"][name] = json{{"type", type_api_key},
									 {"value", cleaned},
									 {"created", utc_now_iso8601()},
									 {"note", note}};
	return store_save(store, data, out_error);
}

bool credential_set_oauth(credential_store &store, const std::string &name,
						  const std::string &access, const json &refresh, const json &expires,
						  const json &note, odin_error &out_error)
{
	const std::string cleaned = trimmed(access);
	if (cleaned.empty())
	{
		fail(out_error, error_kind::config, "refusing to store an empty access token");
		return false;
	}

	json data;
	if (!store_load(store, data, out_error))
		return false;

	data["credentials"][name] = json{{"type", type_oauth},
									 {"access", cleaned},
									 {"refresh", refresh},
									 {"expires", expires},
									 {"created", utc_now_iso8601()},
									 {"note", note}};
	return store_save(store, data, out_error);
}

bool credential_remove(credential_store &store, const std::string &name, odin_error &out_error)
{
	json data;
	if (!store_load(store, data, out_error))
		return false;
	if (data.at("credentials").find(name) == data.at("credentials").end())
		return false;
	data["credentials"].erase(name);
	return store_save(store, data, out_error);
}

bool credential_resolve(const credential_store &store,
						const std::string &credential,
						const std::string &api_key_env,
						std::string &out_secret,
						bool &out_found,
						odin_error &out_error)
{
	out_found = false;

	// environment beats disk, so CI and containers inject without writing a
	// secret to a file
	if (!api_key_env.empty())
	{
		const std::string value = environment_value(api_key_env);
		if (!value.empty())
		{
			out_secret = value;
			out_found = true;
			return true;
		}
	}

	if (!credential.empty())
	{
		std::string variable = "ODIN_CREDENTIAL_";
		for (const char character : credential)
		{
			const unsigned char raw = static_cast<unsigned char>(character);
			// ASCII-only upcasing on purpose: credential names are constrained
			// to ASCII, and a locale-sensitive toupper would make the variable
			// name depend on the machine's locale.
			if (character == '-')
				variable.push_back('_');
			else if (raw < 0x80)
				variable.push_back(static_cast<char>(std::toupper(raw)));
			else
				variable.push_back(character);
		}

		const std::string override_value = environment_value(variable);
		if (!override_value.empty())
		{
			out_secret = override_value;
			out_found = true;
			return true;
		}

		if (!credential_secret(store, credential, out_secret, out_error))
			return false;
		out_found = true;
		return true;
	}

	// an api_key_env that was named but is unset is an error; nothing named at
	// all is simply "no key needed"
	if (!api_key_env.empty())
	{
		fail(out_error, error_kind::config, api_key_env + " is not set");
		return false;
	}
	return true;
}
