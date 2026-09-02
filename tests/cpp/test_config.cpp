#include <doctest/doctest.h>

#include "atomic_file.h"
#include "config.h"
#include "test_support.h"

#include <string>

// the fixture from tests/test_config.py, kept identical so both suites describe
// the same file.
static const char *valid_toml = R"(
[harness]
state_dir = ".odin/runs"
[adapters.mock]
command = ["python", "mock.py", "{model}"]
[models.small]
adapter = "mock"
model = "coder-32b"
parameter_billions = 32
[routing]
default = "small"
[gates.quality]
command = ["python", "-m", "unittest"]
)";

static std::string replaced(std::string text, const std::string &from, const std::string &to)
{
	const std::size_t at = text.find(from);
	if (at != std::string::npos)
		text.replace(at, from.size(), to);
	return text;
}

TEST_CASE("model size is metadata, not a hardcoded threshold")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";
	temp_write(path, valid_toml);

	odin_error err;
	const project_config config = config_load(path, err);
	REQUIRE_FALSE(failed(err));

	const auto profile = config.models.find("small");
	REQUIRE(profile != config.models.end());
	CHECK(profile->second.parameter_billions == json(32));
	// a toml integer must not become a float on the way to the state files
	CHECK(profile->second.parameter_billions.is_number_integer());

	const model_profile *routed = config_model_for(config, "any-agent", "", err);
	REQUIRE_FALSE(failed(err));
	REQUIRE(routed != nullptr);
	CHECK(routed->model == "coder-32b");
}

TEST_CASE("routing to an unknown model fails")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";
	temp_write(path, replaced(valid_toml, "default = \"small\"", "default = \"missing\""));

	odin_error err;
	const project_config config = config_load(path, err);
	REQUIRE_FALSE(failed(err));

	const model_profile *routed = config_model_for(config, "agent", "", err);
	CHECK(routed == nullptr);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::workflow);
	CHECK(err.message == "model profile 'missing' does not exist");
}

TEST_CASE("an explicit override wins over routing")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";
	temp_write(path, valid_toml);

	odin_error err;
	const project_config config = config_load(path, err);
	REQUIRE_FALSE(failed(err));

	const model_profile *routed = config_model_for(config, "any-agent", "small", err);
	REQUIRE_FALSE(failed(err));
	REQUIRE(routed != nullptr);
	CHECK(routed->name == "small");
}

TEST_CASE("routing with no entry and no default is reported against the agent")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";
	temp_write(path, replaced(valid_toml, "default = \"small\"", "reviewer = \"small\""));

	odin_error err;
	const project_config config = config_load(path, err);
	REQUIRE_FALSE(failed(err));

	config_model_for(config, "analyst", "", err);
	REQUIRE(failed(err));
	CHECK(err.message == "no model route configured for agent 'analyst'");
}

TEST_CASE("config_adapter_for resolves and reports a missing adapter")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";
	temp_write(path, valid_toml);

	odin_error err;
	const project_config config = config_load(path, err);
	REQUIRE_FALSE(failed(err));

	const model_profile *profile = config_model_for(config, "any", "", err);
	REQUIRE(profile != nullptr);

	const command_spec *adapter = config_adapter_for(config, *profile, err);
	REQUIRE_FALSE(failed(err));
	REQUIRE(adapter != nullptr);
	CHECK(adapter->command.size() == 3);
	// absent timeout_seconds falls back to the shared default
	CHECK(adapter->timeout_seconds == default_timeout_seconds);

	model_profile broken = *profile;
	broken.adapter = "absent";
	CHECK(config_adapter_for(config, broken, err) == nullptr);
	CHECK(err.message == "adapter 'absent' for model profile 'small' does not exist");
}

TEST_CASE("state_dir is resolved against the config's own directory")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";
	temp_write(path, valid_toml);

	odin_error err;
	const project_config config = config_load(path, err);
	REQUIRE_FALSE(failed(err));

	CHECK(config.state_dir.parent_path().filename() == ".odin");
	CHECK(config.state_dir.filename() == "runs");
#ifdef _WIN32
	// pathlib's str() emits backslashes on windows, and run_dir reaches
	// context.json as a relative string. make_preferred keeps them matching.
	CHECK(file_path_utf8(config.state_dir).find('/') == std::string::npos);
#endif
}

TEST_CASE("a missing configuration is reported the way harness/config.py does")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";

	odin_error err;
	config_load(path, err);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::config);
	CHECK(err.message == "configuration not found: " + file_path_utf8(path));
}

TEST_CASE("harness.max_total_transitions must be a positive integer")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";
	temp_write(path, "[harness]\nmax_total_transitions = 0\n");

	odin_error err;
	config_load(path, err);
	REQUIRE(failed(err));
	CHECK(err.message == "harness.max_total_transitions must be a positive integer");
}

TEST_CASE("a command entry must carry a non-empty string array")
{
	const temp_dir dir;

	SUBCASE("empty command")
	{
		const auto path = dir.path / "odin.toml";
		temp_write(path, "[gates.quality]\ncommand = []\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "command 'gates.quality' must contain a non-empty string array 'command'");
	}

	SUBCASE("non-string element")
	{
		const auto path = dir.path / "odin2.toml";
		temp_write(path, "[gates.quality]\ncommand = [\"python\", 3]\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "command 'gates.quality' must contain a non-empty string array 'command'");
	}

	SUBCASE("non-positive timeout")
	{
		const auto path = dir.path / "odin3.toml";
		temp_write(path, "[adapters.mock]\ncommand = [\"python\"]\ntimeout_seconds = 0\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "command 'adapters.mock'.timeout_seconds must be a positive integer");
	}
}

TEST_CASE("model entries are validated field by field")
{
	const temp_dir dir;

	SUBCASE("adapter and model must be strings")
	{
		const auto path = dir.path / "a.toml";
		temp_write(path, "[models.small]\nadapter = 1\nmodel = \"x\"\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "models.small requires string adapter and model values");
	}

	SUBCASE("parameter_billions must be numeric")
	{
		const auto path = dir.path / "b.toml";
		temp_write(path, "[models.small]\nadapter = \"m\"\nmodel = \"x\"\nparameter_billions = \"32\"\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "models.small.parameter_billions must be numeric");
	}

	SUBCASE("context_tokens must be an integer")
	{
		const auto path = dir.path / "c.toml";
		temp_write(path, "[models.small]\nadapter = \"m\"\nmodel = \"x\"\ncontext_tokens = 1.5\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "models.small.context_tokens must be an integer");
	}

	SUBCASE("tags must be a string array")
	{
		const auto path = dir.path / "d.toml";
		temp_write(path, "[models.small]\nadapter = \"m\"\nmodel = \"x\"\ntags = [1]\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "models.small.tags must be a string array");
	}

	SUBCASE("a float parameter_billions stays a float")
	{
		const auto path = dir.path / "e.toml";
		temp_write(path, "[models.small]\nadapter = \"m\"\nmodel = \"x\"\nparameter_billions = 1.5\n");

		odin_error err;
		const project_config config = config_load(path, err);
		REQUIRE_FALSE(failed(err));
		CHECK(config.models.at("small").parameter_billions.is_number_float());
	}
}

TEST_CASE("routing and environment must be string to string tables")
{
	const temp_dir dir;

	SUBCASE("routing")
	{
		const auto path = dir.path / "a.toml";
		temp_write(path, "[routing]\ndefault = 3\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "routing must be a string-to-string table");
	}

	SUBCASE("environment")
	{
		const auto path = dir.path / "b.toml";
		temp_write(path, "[environment]\nPATH = 3\n");

		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "environment must be a string-to-string table");
	}
}

TEST_CASE("git.stage_on_success defaults to false")
{
	const temp_dir dir;

	SUBCASE("absent")
	{
		const auto path = dir.path / "a.toml";
		temp_write(path, valid_toml);

		odin_error err;
		const project_config config = config_load(path, err);
		REQUIRE_FALSE(failed(err));
		CHECK_FALSE(config.stage_on_success);
		CHECK(config.git_timeout_seconds == default_timeout_seconds);
		CHECK(config.max_total_transitions == default_max_total_transitions);
	}

	SUBCASE("enabled")
	{
		const auto path = dir.path / "b.toml";
		temp_write(path, "[git]\nstage_on_success = true\n");

		odin_error err;
		const project_config config = config_load(path, err);
		REQUIRE_FALSE(failed(err));
		CHECK(config.stage_on_success);
	}

	SUBCASE("timeout")
	{
		const auto path = dir.path / "c.toml";
		temp_write(path, "[git]\ntimeout_seconds = 17\n");
		odin_error err;
		const project_config config = config_load(path, err);
		REQUIRE_FALSE(failed(err));
		CHECK(config.git_timeout_seconds == 17);
	}

	SUBCASE("invalid timeout")
	{
		const auto path = dir.path / "d.toml";
		temp_write(path, "[git]\ntimeout_seconds = 0\n");
		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "git.timeout_seconds must be a positive integer");
	}

	SUBCASE("overflow timeout")
	{
		const auto path = dir.path / "e.toml";
		temp_write(path, "[git]\ntimeout_seconds = 2147483648\n");
		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "git.timeout_seconds must be a positive integer");
	}
}

TEST_CASE("commands declare inherited and literal environment")
{
	const temp_dir dir;
	const auto path = dir.path / "environment.toml";
	temp_write(path,
			   "[adapters.test]\ncommand = [\"python\"]\n"
			   "inherit_environment = [\"OPENAI_API_KEY\"]\n"
			   "environment = { PYTHONUTF8 = \"1\" }\n");
	odin_error err;
	const project_config config = config_load(path, err);
	REQUIRE_FALSE(failed(err));
	CHECK(config.adapters.at("test").inherit_environment ==
		  std::vector<std::string>{"OPENAI_API_KEY"});
	CHECK(config.adapters.at("test").environment.at("PYTHONUTF8") == "1");
}

TEST_CASE("a built-in adapter kind needs no command")
{
	const temp_dir dir;
	const auto path = dir.path / "odin.toml";

	SUBCASE("type mock parses and carries no argv")
	{
		temp_write(path, "[adapters.mock]\ntype = \"mock\"\n");
		odin_error err;
		const project_config config = config_load(path, err);
		REQUIRE_FALSE(failed(err));
		CHECK(config.adapters.at("mock").type == "mock");
		CHECK(config.adapters.at("mock").command.empty());
		CHECK(command_spec_is_builtin(config.adapters.at("mock")));
	}

	SUBCASE("an adapter without a type is still an external command")
	{
		temp_write(path, "[adapters.custom]\ncommand = [\"my-adapter\"]\n");
		odin_error err;
		const project_config config = config_load(path, err);
		REQUIRE_FALSE(failed(err));
		CHECK(config.adapters.at("custom").type == "command");
		CHECK_FALSE(command_spec_is_builtin(config.adapters.at("custom")));
	}

	SUBCASE("an unknown type is refused rather than treated as a command")
	{
		// silently falling back would turn a typo into a confusing
		// missing-executable failure in the middle of a run
		temp_write(path, "[adapters.x]\ntype = \"openai\"\n");
		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message == "command 'adapters.x'.type 'openai' is not a built-in adapter kind");
	}

	SUBCASE("a built-in kind may not also declare a command")
	{
		temp_write(path, "[adapters.x]\ntype = \"mock\"\ncommand = [\"anything\"]\n");
		odin_error err;
		config_load(path, err);
		REQUIRE(failed(err));
		CHECK(err.message.find("must not also set 'command'") != std::string::npos);
	}
}