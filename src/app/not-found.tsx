import { redirect } from 'next/navigation';

// Mirrors the old react-router catch-all (`path="*"` -> Navigate to /dashboard).
export default function NotFound() {
  redirect('/dashboard');
}
