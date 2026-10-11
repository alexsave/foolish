// The product's domain, in the QR code the lobby shows and in the synthetic
// address an anonymous account is registered under.
//
// A module of its own because e2e/lobby_add_bot.test.ts mocks this exact path
// to point the QR code at example.com.
export const WEBSITE_DOMAIN = 'foolish.cards';
