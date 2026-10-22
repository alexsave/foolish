import type { Metadata, Viewport } from 'next';
import './globals.css';
import './pieces.css';
import Top from '@/components/Top';

export const metadata: Metadata = {
  title: 'c_chess',
  description: 'Play chess, fast. Free, no ads.',
};
export const viewport: Viewport = { width: 'device-width', initialScale: 1 };

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>
        <Top />
        <main>{children}</main>
      </body>
    </html>
  );
}
