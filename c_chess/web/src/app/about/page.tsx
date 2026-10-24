// About: what this is, and the credits its licenses ask for.
export default function About() {
  return (
    <div className="panel" style={{ padding: 24, maxWidth: 760 }}>
      <h1>About c_chess</h1>
      <p>
        A chess site built for speed: one small server process written in C holds every game in memory, and your browser
        runs the same C rules compiled to WebAssembly, so legal moves show at once and a move travels as a few bytes.
      </p>
      <h2>Credits and licenses</h2>
      <ul>
        <li>c_chess is free software under the GNU General Public License, version 3.</li>
        <li>The computer opponent is <a href="https://stockfishchess.org">Stockfish</a> 19, GPLv3, built unmodified from
          its <a href="https://github.com/official-stockfish/Stockfish/tree/sf_19">sf_19 tag</a> (<a href="https://github.com/official-stockfish/Stockfish/blob/sf_19/Copying.txt">license</a>);
          it runs on the server as a separate program.</li>
        <li>Pieces: the &quot;cburnett&quot; set by <a href="https://en.wikipedia.org/wiki/User:Cburnett">Colin M.L. Burnett</a>, GPLv2 or later.</li>
      </ul>
    </div>
  );
}
