#include <doctest/doctest.h>

#include "credentials.h"
#include "json_io.h"
#include "test_support.h"

#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace
{

credential_store store_in(const std::filesystem::path &root)
{
	credential_store store;
	credential_store_configure(store, root);
	return store;
}

void set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

void clear_env(const char *name)
{
#ifdef _WIN32
	_putenv_s(name, "");
#else
	unsetenv(name);
#endif
}

} // namespace

// ------------------------------------------------------------------ mask

TEST_CASE("credential_mask never reveals a short secret")
{
	CHECK(credential_mask("") == "<empty>");
	// at or below twelve characters the value is replaced entirely: showing
	// three of eight would give most of it away
	CHECK(credential_mask("abc123") == "******");
	CHECK(credential_mask("123456789012") == "************");
	CHECK(credential_mask("1234567890123") == "123...0123");
	CHECK(credential_mask("sk-or-v1-abcdefghijklmnop") == "sk-...mnop");
}

TEST_CASE("credential_mask slices on character boundaries")
{
	// six 2-byte characters: 12 characters, 24 bytes. a byte-based length check
	// would treat this as long and then slice mid-sequence, emitting invalid
	// UTF-8 into something that later has to be serialised as JSON.
	const std::string twelve =
	  "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
	CHECK(credential_mask(twelve) == "************");

	const std::string thirteen = twelve + "\xc3\xa9";
	const std::string masked = credential_mask(thirteen);
	// 3 characters + "..." + 4 characters, all whole
	CHECK(masked == "\xc3\xa9\xc3\xa9\xc3\xa9...\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");
	CHECK(json(masked).dump().size() > 0); // serialisable, i.e. valid UTF-8
}

// ---------------------------------------------------------------- redact

TEST_CASE("credential_redact catches every rendering of a secret")
{
	const std::string secret = "sk-secret-value";

	SUBCASE("the raw value")
	{
		CHECK(credential_redact("key=" + secret + " done", secret) == "key=[REDACTED] done");
	}

	SUBCASE("all occurrences, not just the first")
	{
		CHECK(credential_redact(secret + "|" + secret, secret) == "[REDACTED]|[REDACTED]");
	}

	SUBCASE("an empty secret redacts nothing")
	{
		CHECK(credential_redact("untouched", "") == "untouched");
	}
}

TEST_CASE("credential_redact catches a JSON-escaped secret")
{
	// the case a raw-bytes-only check misses: the secret reached the log after
	// being serialised, so its quotes and backslashes are escaped
	const std::string secret = "quote-\"-slash-\\-secret";
	const std::string escaped = json(secret).dump();
	REQUIRE(escaped.find("\\\"") != std::string::npos);

	CHECK(credential_redact(escaped, secret) == "\"[REDACTED]\"");
}

TEST_CASE("credential_redact catches an ascii-escaped unicode secret")
{
	// ensure_ascii output: the bytes on disk are caf\u00e9-secret, which shares
	// no substring with the raw utf-8 value
	const std::string secret = "caf\xc3\xa9-secret-value";
	const std::string ascii = json(secret).dump(-1, ' ', true);
	REQUIRE(ascii.find("\\u00e9") != std::string::npos);

	const std::string redacted = credential_redact(ascii, secret);
	CHECK(redacted.find("\\u00e9") == std::string::npos);
	CHECK(redacted.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("credential_redact replaces the longest form first")
{
	// a secret whose escaped form contains its raw form. replacing the short
	// one first would leave a partially-redacted fragment of the long one.
	const std::string secret = "abc\\def";
	const std::string escaped = json(secret).dump(); // "abc\\def" -> abc\\def
	const std::string redacted = credential_redact(escaped, secret);
	CHECK(redacted.find("def") == std::string::npos);
}

// ----------------------------------------------------------------- store

TEST_CASE("an absent store reads as empty rather than failing")
{
	const temp_dir dir;
	const credential_store store = store_in(dir.path);

	odin_error err;
	CHECK(credential_names(store, err).empty());
	CHECK_FALSE(failed(err));
	CHECK(credential_describe_all(store, err).empty());
	CHECK_FALSE(failed(err));
}

TEST_CASE("a stored api key round-trips and is masked when described")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);

	odin_error err;
	REQUIRE(credential_set_api_key(store, "openrouter", "sk-or-v1-abcdefghijklmnop",
								   json("hosted"), err));
	REQUIRE_FALSE(failed(err));

	json described;
	REQUIRE(credential_describe(store, "openrouter", described, err));
	CHECK(described.at("name") == "openrouter");
	CHECK(described.at("type") == "api_key");
	CHECK(described.at("note") == "hosted");
	CHECK(described.at("value") == "sk-...mnop");
	// created is recorded as UTC seconds with an explicit offset
	CHECK(described.at("created").get<std::string>().size() == 25);
	CHECK(described.at("created").get<std::string>().find("+00:00") != std::string::npos);

	// the raw value comes only from the explicit accessor
	std::string secret;
	REQUIRE(credential_secret(store, "openrouter", secret, err));
	CHECK(secret == "sk-or-v1-abcdefghijklmnop");
}

TEST_CASE("the store is version 1 on disk, so existing files stay readable")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);

	odin_error err;
	REQUIRE(credential_set_api_key(store, "k", "value-long-enough", json(nullptr), err));

	const json raw = json_read(credential_store_path(store), err);
	REQUIRE_FALSE(failed(err));
	CHECK(raw.at("version") == 1);
	CHECK(raw.at("credentials").at("k").at("type") == "api_key");
	CHECK(raw.at("credentials").at("k").at("value") == "value-long-enough");
}

TEST_CASE("a store written by the previous implementation is read unchanged")
{
	// a literal v1 file, exactly as the previous implementation left it.
	// users must not lose credentials to the port.
	const temp_dir dir;
	temp_write(dir.path / ".odin" / "credentials.json", R"({
	  "version": 1,
	  "credentials": {
	    "legacy-key": {
	      "type": "api_key",
	      "value": "sk-legacy-abcdefghijkl",
	      "created": "2026-08-31T00:00:00+00:00",
	      "note": "written by python"
	    },
	    "legacy-oauth": {
	      "type": "oauth",
	      "access": "gho_legacyaccesstoken",
	      "refresh": "ghr_legacyrefresh",
	      "expires": 4102444800,
	      "created": "2026-08-31T00:00:00+00:00",
	      "note": null
	    }
	  }
	})");

	const credential_store store = store_in(dir.path);
	odin_error err;

	const std::vector<std::string> names = credential_names(store, err);
	REQUIRE_FALSE(failed(err));
	REQUIRE(names.size() == 2);
	CHECK(names[0] == "legacy-key");
	CHECK(names[1] == "legacy-oauth");

	std::string secret;
	REQUIRE(credential_secret(store, "legacy-key", secret, err));
	CHECK(secret == "sk-legacy-abcdefghijkl");

	// an oauth entry resolves to its access token, not its whole record
	REQUIRE(credential_secret(store, "legacy-oauth", secret, err));
	CHECK(secret == "gho_legacyaccesstoken");

	json described;
	REQUIRE(credential_describe(store, "legacy-oauth", described, err));
	CHECK(described.at("type") == "oauth");
	CHECK(described.at("expired") == false); // 2100
	CHECK(described.at("expires") == 4102444800);
}

TEST_CASE("names are sorted and describe_all follows them")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);
	odin_error err;
	for (const char *name : {"zulu", "alpha", "mike"})
		REQUIRE(credential_set_api_key(store, name, "value-long-enough", json(nullptr), err));

	const std::vector<std::string> names = credential_names(store, err);
	CHECK(names == std::vector<std::string>{"alpha", "mike", "zulu"});

	const json all = credential_describe_all(store, err);
	REQUIRE(all.size() == 3);
	CHECK(all.at(0).at("name") == "alpha");
	CHECK(all.at(2).at("name") == "zulu");
}

TEST_CASE("describe reports a missing credential without failing")
{
	const temp_dir dir;
	const credential_store store = store_in(dir.path);
	odin_error err;
	json described;
	CHECK_FALSE(credential_describe(store, "absent", described, err));
	CHECK_FALSE(failed(err));
}

TEST_CASE("remove reports a miss without failing")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);
	odin_error err;
	REQUIRE(credential_set_api_key(store, "present", "value-long-enough", json(nullptr), err));

	CHECK(credential_remove(store, "present", err));
	CHECK_FALSE(failed(err));
	CHECK_FALSE(credential_remove(store, "present", err));
	CHECK_FALSE(failed(err));
}

TEST_CASE("an empty secret is refused rather than stored")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);
	odin_error err;

	CHECK_FALSE(credential_set_api_key(store, "k", "   ", json(nullptr), err));
	REQUIRE(failed(err));
	CHECK(err.message == "refusing to store an empty credential");
	// and nothing was written
	CHECK_FALSE(std::filesystem::exists(credential_store_path(store)));
}

TEST_CASE("a malformed store is reported, not silently treated as empty")
{
	const temp_dir dir;
	temp_write(dir.path / ".odin" / "credentials.json", R"({"version":1,"credentials":[]})");

	const credential_store store = store_in(dir.path);
	odin_error err;
	credential_names(store, err);
	REQUIRE(failed(err));
	CHECK(err.message == "credential store is malformed: 'credentials' is not an object");
}

// ---------------------------------------------------------------- expiry

TEST_CASE("expiry is tri-state")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);
	odin_error err;

	// an api key has no expiry recorded at all, which is not the same as "not
	// expired" - the CLI renders the two differently
	REQUIRE(credential_set_api_key(store, "key", "value-long-enough", json(nullptr), err));
	CHECK(credential_is_expired(store, "key", err) == credential_expiry::unknown);

	REQUIRE(credential_set_oauth(store, "live", "token-value", json(nullptr), json(4102444800),
								 json(nullptr), err));
	CHECK(credential_is_expired(store, "live", err) == credential_expiry::valid);

	REQUIRE(credential_set_oauth(store, "stale", "token-value", json(nullptr), json(1),
								 json(nullptr), err));
	CHECK(credential_is_expired(store, "stale", err) == credential_expiry::expired);

	CHECK(credential_is_expired(store, "absent", err) == credential_expiry::unknown);
}

TEST_CASE("an expired token is refused loudly instead of being sent")
{
	// otherwise the failure surfaces as an opaque provider 401, long after the
	// point where the user could have been told what to do
	const temp_dir dir;
	credential_store store = store_in(dir.path);
	odin_error err;
	REQUIRE(credential_set_oauth(store, "stale", "token-value", json(nullptr), json(1),
								 json(nullptr), err));

	std::string secret;
	CHECK_FALSE(credential_secret(store, "stale", secret, err));
	REQUIRE(failed(err));
	CHECK(err.message.find("has expired") != std::string::npos);
	CHECK(err.message.find("odin auth set stale") != std::string::npos);
}

TEST_CASE("a missing credential names the command that would create it")
{
	const temp_dir dir;
	const credential_store store = store_in(dir.path);
	odin_error err;
	std::string secret;
	CHECK_FALSE(credential_secret(store, "absent", secret, err));
	REQUIRE(failed(err));
	CHECK(err.message == "no credential named 'absent'. Add one with: odin auth set absent");
}

// --------------------------------------------------------------- resolve

TEST_CASE("credential_resolve prefers the environment over the store")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);
	odin_error err;
	REQUIRE(credential_set_api_key(store, "provider", "from-the-store", json(nullptr), err));

	std::string secret;
	bool found = false;

	SUBCASE("a named api key variable wins outright")
	{
		set_env("ODIN_TEST_API_KEY", "from-the-named-variable");
		REQUIRE(credential_resolve(store, "provider", "ODIN_TEST_API_KEY", secret, found, err));
		CHECK(found);
		CHECK(secret == "from-the-named-variable");
		clear_env("ODIN_TEST_API_KEY");
	}

	SUBCASE("then the per-credential override")
	{
		clear_env("ODIN_TEST_API_KEY");
		set_env("ODIN_CREDENTIAL_PROVIDER", "from-the-override");
		REQUIRE(credential_resolve(store, "provider", "ODIN_TEST_API_KEY", secret, found, err));
		CHECK(secret == "from-the-override");
		clear_env("ODIN_CREDENTIAL_PROVIDER");
	}

	SUBCASE("then the store")
	{
		clear_env("ODIN_TEST_API_KEY");
		clear_env("ODIN_CREDENTIAL_PROVIDER");
		REQUIRE(credential_resolve(store, "provider", "", secret, found, err));
		CHECK(secret == "from-the-store");
	}
}

TEST_CASE("credential_resolve maps a hyphenated name to an underscored variable")
{
	const temp_dir dir;
	const credential_store store = store_in(dir.path);
	set_env("ODIN_CREDENTIAL_GITHUB_COPILOT", "from-the-override");

	odin_error err;
	std::string secret;
	bool found = false;
	REQUIRE(credential_resolve(store, "github-copilot", "", secret, found, err));
	CHECK(found);
	CHECK(secret == "from-the-override");
	clear_env("ODIN_CREDENTIAL_GITHUB_COPILOT");
}

TEST_CASE("credential_resolve distinguishes no key needed from a missing one")
{
	const temp_dir dir;
	const credential_store store = store_in(dir.path);
	odin_error err;
	std::string secret;
	bool found = false;

	SUBCASE("nothing named at all is success with no secret")
	{
		// a local server needs no key; this must not be an error
		REQUIRE(credential_resolve(store, "", "", secret, found, err));
		CHECK_FALSE(found);
		CHECK_FALSE(failed(err));
	}

	SUBCASE("a named variable that is unset is an error")
	{
		clear_env("ODIN_TEST_ABSENT_KEY");
		CHECK_FALSE(credential_resolve(store, "", "ODIN_TEST_ABSENT_KEY", secret, found, err));
		REQUIRE(failed(err));
		CHECK(err.message == "ODIN_TEST_ABSENT_KEY is not set");
	}
}

// ----------------------------------------------------------- permissions

#ifndef _WIN32
TEST_CASE("the store is owner-only on posix")
{
	const temp_dir dir;
	credential_store store = store_in(dir.path);
	odin_error err;
	REQUIRE(credential_set_api_key(store, "k", "value-long-enough", json(nullptr), err));

	struct stat info {};
	REQUIRE(::stat(credential_store_path(store).c_str(), &info) == 0);
	CHECK((info.st_mode & 0777) == 0600);
}
#endif
