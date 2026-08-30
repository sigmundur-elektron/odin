#include "contracts.h"

void contract_validate(sidecar &service,
					   const json &value,
					   const std::string &contract,
					   const std::string &where,
					   odin_error &out_error)
{

	json request;
	request["op"] = "validate";
	request["contract"] = contract;
	request["where"] = where;
	request["value"] = value;

	const json reply = sidecar_call(service, std::move(request), out_error);
	if (failed(out_error))
		return;

	const auto ok = reply.find("ok");
	if (ok != reply.end() && ok->is_boolean() && ok->get<bool>())
		return;

	// the service always explains itself; the fallback is for a protocol defect.
	std::string message = "contract '" + contract + "' could not be checked";
	const auto reported = reply.find("error");
	if (reported != reply.end() && reported->is_string())
		message = reported->get<std::string>();

	fail(out_error, error_kind::contract, message);
}
