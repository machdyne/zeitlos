#
# Importing this package registers every adapter. `ask sources` lists
# what came back, which is the check that a new adapter file is
# actually being picked up rather than silently absent.
#
from . import codex   # noqa: F401
from . import basic   # noqa: F401
