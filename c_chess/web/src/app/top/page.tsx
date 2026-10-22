// The leaderboard, in full.
import Leaders from '@/components/Leaders';

export default function Top() {
  return (
    <section className="panel" style={{ maxWidth: 640 }}>
      <Leaders limit={50} />
    </section>
  );
}
