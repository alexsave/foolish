-- Lease RENEWAL, so a bot loop can run long (adaptive CPU budgeting) while keeping
-- the lease TTL SHORT (fast recovery if the isolate is hard-killed). The loop calls
-- this each cycle to extend its own lease (fenced on the token). Returns false if we
-- no longer hold the lease (someone else took over) so the loop can stop.
CREATE OR REPLACE FUNCTION renew_bot_lease(p_game_id TEXT, p_token UUID, p_ttl_ms INT)
RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_rows INT;
BEGIN
  UPDATE games SET bot_lease_until = now() + make_interval(secs => p_ttl_ms / 1000.0)
  WHERE id = p_game_id AND bot_lease_token = p_token;
  GET DIAGNOSTICS v_rows = ROW_COUNT;
  RETURN v_rows > 0;
END;
$$;
