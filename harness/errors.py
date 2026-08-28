class OdinError(Exception):
    """A user-actionable harness error."""


class ContractError(OdinError):
    """Input or agent output did not satisfy its JSON contract."""


class AdapterError(OdinError):
    """The configured agent adapter could not produce a valid response."""


class WorkflowError(OdinError):
    """A workflow definition or transition is invalid."""
