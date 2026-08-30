#include "config.h"

#include <toml++/toml.hpp>

#include "atomic_file.h"

namespace fs = std::filesystem;

// every message below is reproduced verbatim from harness/config.py. they are
// asserted by the parity tests and are user-facing product surface.

static bool command_spec_read(const std::string &name,
							  const toml::node &node,
							  command_spec &out_spec,
							  odin_error &out_error)
{

	const toml::table *table = node.as_table();
	if (!table)
	{
		fail(out_error, error_kind::config, "command '" + name + "' must be a table");
		return false;
	}

	const toml::array *command = (*table)["command"].as_array();
	bool usable = command != nullptr && !command->empty();
	if (usable)
	{
		for (const toml::node &part : *command)
		{
			if (!part.is_string())
			{
				usable = false;
				break;
			}
		}
	}
	if (!usable)
	{
		fail(out_error, error_kind::config,
			 "command '" + name + "' must contain a non-empty string array 'command'");
		return false;
	}
	for (const toml::node &part : *command) out_spec.command.push_back(part.value_or(std::string{}));

	const toml::node *timeout = table->get("timeout_seconds");
	if (timeout == nullptr)
	{
		out_spec.timeout_seconds = default_timeout_seconds;
		return true;
	}
	const std::optional<std::int64_t> seconds = timeout->value<std::int64_t>();
	if (!timeout->is_integer() || !seconds.has_value() || *seconds < 1)
	{
		fail(out_error, error_kind::config,
			 "command '" + name + "'.timeout_seconds must be a positive integer");
		return false;
	}
	out_spec.timeout_seconds = static_cast<int>(*seconds);
	return true;
}

// a toml integer must stay an integer in json. python's tomllib makes the same
// distinction, and it reaches the state files through adapter metadata.
static json numeric_to_json(const toml::node &node)
{
	if (node.is_integer())
		return json(node.value_or(std::int64_t{0}));
	return json(node.value_or(0.0));
}

static bool string_map_read(const std::string &label,
							const toml::table &root,
							const char *key,
							std::map<std::string, std::string> &out_map,
							odin_error &out_error)
{

	const toml::node *node = root.get(key);
	if (node == nullptr)
		return true;

	const toml::table *table = node->as_table();
	if (!table)
	{
		fail(out_error, error_kind::config, label);
		return false;
	}
	for (const auto &[name, value] : *table)
	{
		if (!value.is_string())
		{
			fail(out_error, error_kind::config, label);
			return false;
		}
		out_map.emplace(std::string(name.str()), value.value_or(std::string{}));
	}
	return true;
}

static bool command_table_read(const std::string &prefix,
							   const toml::table &root,
							   const char *key,
							   std::map<std::string, command_spec> &out_map,
							   odin_error &out_error)
{

	const toml::node *node = root.get(key);
	if (node == nullptr)
		return true;

	const toml::table *table = node->as_table();
	if (!table)
		return true; // python iterates .items() on a non-dict and yields nothing usable

	for (const auto &[name, value] : *table)
	{
		command_spec spec;
		if (!command_spec_read(prefix + std::string(name.str()), value, spec, out_error))
			return false;
		out_map.emplace(std::string(name.str()), std::move(spec));
	}
	return true;
}

static bool models_read(const toml::table &root,
						std::map<profile_id, model_profile> &out_models,
						odin_error &out_error)
{

	const toml::node *node = root.get("models");
	if (node == nullptr)
		return true;

	const toml::table *table = node->as_table();
	if (!table)
		return true;

	for (const auto &[key, value] : *table)
	{
		const std::string name(key.str());

		const toml::table *entry = value.as_table();
		if (!entry)
		{
			fail(out_error, error_kind::config, "models." + name + " must be a table");
			return false;
		}

		model_profile profile;
		profile.name = name;

		const toml::node *adapter = entry->get("adapter");
		const toml::node *model = entry->get("model");
		if (adapter == nullptr || !adapter->is_string() || model == nullptr || !model->is_string())
		{
			fail(out_error, error_kind::config,
				 "models." + name + " requires string adapter and model values");
			return false;
		}
		profile.adapter = adapter->value_or(std::string{});
		profile.model = model->value_or(std::string{});

		if (const toml::node *size = entry->get("parameter_billions"))
		{
			if (!size->is_integer() && !size->is_floating_point())
			{
				fail(out_error, error_kind::config,
					 "models." + name + ".parameter_billions must be numeric");
				return false;
			}
			profile.parameter_billions = numeric_to_json(*size);
		}

		if (const toml::node *context = entry->get("context_tokens"))
		{
			if (!context->is_integer())
			{
				fail(out_error, error_kind::config,
					 "models." + name + ".context_tokens must be an integer");
				return false;
			}
			profile.context_tokens = json(context->value_or(std::int64_t{0}));
		}

		if (const toml::node *tags = entry->get("tags"))
		{
			const toml::array *list = tags->as_array();
			if (!list)
			{
				fail(out_error, error_kind::config, "models." + name + ".tags must be a string array");
				return false;
			}
			for (const toml::node &tag : *list)
			{
				if (!tag.is_string())
				{
					fail(out_error, error_kind::config,
						 "models." + name + ".tags must be a string array");
					return false;
				}
				profile.tags.push_back(tag.value_or(std::string{}));
			}
		}

		out_models.emplace(name, std::move(profile));
	}
	return true;
}

project_config config_load(const fs::path &path, odin_error &out_error)
{
	project_config config;

	odin_error read_error;
	const std::string contents = file_read_all(path, read_error);
	if (failed(read_error))
	{
		fail(out_error, error_kind::config, "configuration not found: " + file_path_utf8(path));
		return config;
	}

	// note: toml++'s diagnostics do not match tomllib's TOMLDecodeError text.
	// accepted divergence - it only surfaces on a malformed hand-edited file.
	toml::parse_result parsed = toml::parse(contents);
	if (!parsed)
	{
		fail(out_error, error_kind::config,
			 "invalid TOML in " + file_path_utf8(path) + ": " + std::string(parsed.error().description()));
		return config;
	}
	const toml::table &root = parsed.table();

	std::error_code code;
	fs::path resolved = fs::weakly_canonical(path, code);
	if (code)
		resolved = fs::absolute(path);
	config.root = resolved.parent_path();

	std::string state_dir = ".odin/runs";
	if (const toml::node *harness = root.get("harness"))
	{
		if (const toml::table *table = harness->as_table())
		{
			if (const toml::node *configured = table->get("state_dir"))
			{
				if (configured->is_string())
					state_dir = configured->value_or(std::string{});
			}
			if (const toml::node *maximum = table->get("max_total_transitions"))
			{
				const std::optional<std::int64_t> value = maximum->value<std::int64_t>();
				if (!maximum->is_integer() || !value.has_value() || *value < 1)
				{
					fail(out_error, error_kind::config,
						 "harness.max_total_transitions must be a positive integer");
					return config;
				}
				config.max_total_transitions = static_cast<int>(*value);
			}
		}
	}
	// make_preferred, not lexically_normal: python's Path only swaps separators
	// here, and run_dir is written to context.json as a relative string.
	config.state_dir = (config.root / state_dir).make_preferred();

	if (!command_table_read("adapters.", root, "adapters", config.adapters, out_error))
		return config;
	if (!command_table_read("gates.", root, "gates", config.gates, out_error))
		return config;
	if (!models_read(root, config.models, out_error))
		return config;

	if (!string_map_read("routing must be a string-to-string table",
						 root, "routing", config.routing, out_error))
		return config;
	if (!string_map_read("environment must be a string-to-string table",
						 root, "environment", config.environment, out_error))
		return config;

	if (const toml::node *git = root.get("git"))
	{
		if (const toml::table *table = git->as_table())
		{
			config.stage_on_success = (*table)["stage_on_success"].value_or(false);
		}
	}

	return config;
}

const model_profile *config_model_for(const project_config &config,
									  const agent_id &agent,
									  const std::string &override_name,
									  odin_error &out_error)
{

	// python treats an empty string as absent here, so an empty override falls
	// through to routing rather than failing.
	std::string name = override_name;
	if (name.empty())
	{
		const auto route = config.routing.find(agent);
		if (route != config.routing.end())
			name = route->second;
	}
	if (name.empty())
	{
		const auto fallback = config.routing.find("default");
		if (fallback != config.routing.end())
			name = fallback->second;
	}
	if (name.empty())
	{
		fail(out_error, error_kind::workflow,
			 "no model route configured for agent '" + agent + "'");
		return nullptr;
	}

	const auto profile = config.models.find(name);
	if (profile == config.models.end())
	{
		fail(out_error, error_kind::workflow, "model profile '" + name + "' does not exist");
		return nullptr;
	}
	return &profile->second;
}

const command_spec *config_adapter_for(const project_config &config,
									   const model_profile &profile,
									   odin_error &out_error)
{

	const auto adapter = config.adapters.find(profile.adapter);
	if (adapter == config.adapters.end())
	{
		fail(out_error, error_kind::workflow,
			 "adapter '" + profile.adapter + "' for model profile '" + profile.name + "' does not exist");
		return nullptr;
	}
	return &adapter->second;
}
