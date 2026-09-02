#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

// Credential storage for model providers.
//
// Agents and subagents need model access at run time, so Odin stores provider
// credentials itself rather than depending on whichever vendor CLI happens to be
// installed. The store is project-local, lives at .odin/credentials.json, and is
// never committed.
//
// Three security properties this module owns:
//
//  * Secrets never appear in argv. Adapters receive a credential *name* and read
//    the value themselves; process listings are world-readable on most systems.
//  * Secrets never appear in output. Everything that renders a credential goes
//    through credential_mask; the raw value comes only from credential_secret.
//  * Owner-only file mode. 0600 on POSIX, an explicit current-user DACL on
//    Windows - which is stricter than the Python implementation, whose Windows
//    branch was a documented no-op that inherited the profile ACL.

constexpr int credential_store_version = 1;

struct credential_store
{
	std::filesystem::path root; // the project root; the store hangs off it
};

void credential_store_configure(credential_store &store, const std::filesystem::path &root);
std::filesystem::path credential_store_path(const credential_store &store);

// Render a secret safe for logs, reports, and GUI display.
//
// Short values are replaced entirely rather than partially revealed. Length is
// counted in characters, not bytes: slicing a UTF-8 secret by byte can split a
// sequence and emit invalid UTF-8 into a report that later has to be JSON.
std::string credential_mask(const std::string &value);

// Replace every rendering of `secret` in `text` with [REDACTED].
//
// Three forms are replaced, longest first: the raw value, its ASCII-escaped JSON
// form, and its minimally-escaped JSON form. A secret that reaches a log after
// being JSON-encoded would otherwise sail past a raw-bytes-only check - which is
// not a simplification but a hole in a security control.
std::string credential_redact(const std::string &text, const std::string &secret);

// Sorted credential names. An absent store is empty, not an error.
std::vector<std::string> credential_names(const credential_store &store, odin_error &out_error);

// Metadata plus a masked value; safe to print. Returns false when absent.
bool credential_describe(const credential_store &store, const std::string &name,
						 json &out_described, odin_error &out_error);

json credential_describe_all(const credential_store &store, odin_error &out_error);

// Tri-state, matching the Python contract: expired, not expired, or "no usable
// expiry recorded". The callers render all three differently.
enum class credential_expiry : std::uint8_t
{
	unknown = 0,
	valid,
	expired
};

credential_expiry credential_is_expired(const credential_store &store, const std::string &name,
										odin_error &out_error);

// The only function that exposes a raw value.
bool credential_secret(const credential_store &store, const std::string &name,
					   std::string &out_secret, odin_error &out_error);

bool credential_set_api_key(credential_store &store, const std::string &name,
							const std::string &value, const json &note, odin_error &out_error);

bool credential_set_oauth(credential_store &store, const std::string &name,
						  const std::string &access, const json &refresh, const json &expires,
						  const json &note, odin_error &out_error);

// Returns false when there was nothing to remove; that is not an error.
bool credential_remove(credential_store &store, const std::string &name, odin_error &out_error);

// Resolve a provider secret.
//
// Environment first, so containers and CI can inject a value without writing it
// to disk, then the stored credential. `out_found` is false when neither is
// configured and none was demanded, which is valid for a local server that needs
// no key at all.
bool credential_resolve(const credential_store &store,
						const std::string &credential,
						const std::string &api_key_env,
						std::string &out_secret,
						bool &out_found,
						odin_error &out_error);
