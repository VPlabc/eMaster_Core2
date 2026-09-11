-- state.lua
-- Workflow state definitions.

local State = {}

State.IDLE = "IDLE"
State.VERIFY_CITIZEN = "VERIFY_CITIZEN"
State.MOVE_CARD_TO_SCAN = "MOVE_CARD_TO_SCAN"
State.WAIT_CARD = "WAIT_CARD"
State.REGISTER_CARD = "REGISTER_CARD"
State.MOVE_CARD_OUT = "MOVE_CARD_OUT"
State.WAIT_USER_TAKE = "WAIT_USER_TAKE"
State.COLLECT_CARD = "COLLECT_CARD"
-- Card kept inside the machine (not binned) so the next employee gets it.
State.HOLD_CARD = "HOLD_CARD"
State.FINISH = "FINISH"
State.ERROR = "ERROR"

return State
