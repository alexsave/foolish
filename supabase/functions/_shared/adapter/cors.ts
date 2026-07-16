export const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Headers': 'authorization, x-client-info, apikey, content-type',
}

export const handleCors = (req: Request): Response | null => {
  if (req.method === 'OPTIONS') {
    // Cache the CORS preflight. Without this the browser re-sends an OPTIONS
    // before (nearly) every request — and each preflight can COLD-START the edge
    // function just to answer it, which is the ~2s gap seen between the OPTIONS
    // and the POST. With a Max-Age the browser preflights once, then sends the
    // real request directly for the cached window. 7200s is Chrome's cap.
    return new Response('ok', { headers: { ...corsHeaders, 'Access-Control-Max-Age': '7200' } })
  }
  return null
}
