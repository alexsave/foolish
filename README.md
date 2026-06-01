# Foolish

The web client for Foolish — a дурак (Durak) card game. Built with
[Next.js](https://nextjs.org) (App Router) and Supabase.

## Available Scripts

In the project directory, you can run:

### `npm run dev`

Runs the app in development mode. Open [http://localhost:3000](http://localhost:3000)
to view it in your browser. The page reloads on changes.

### `npm run build`

Builds the app for production to the `.next` folder.

### `npm start`

Runs the production build (after `npm run build`).

## Environment

Client-exposed variables use the `NEXT_PUBLIC_` prefix:

- `NEXT_PUBLIC_SUPABASE_URL`
- `NEXT_PUBLIC_SUPABASE_KEY`

## Deployment

Deploys to Vercel with zero configuration — Vercel auto-detects Next.js and
serves the `.next` build output.

## Routing

The App Router maps to the previous react-router routes:

| Path         | Page                                                |
| ------------ | --------------------------------------------------- |
| `/`          | Welcome (redirects signed-in users to `/dashboard`) |
| `/about`     | About                                               |
| `/tutorial`  | Tutorial                                            |
| `/dashboard` | Dashboard (auth required)                           |
| `/:game_id`  | Game view (auth required)                           |
