const KEYS: ReadonlyArray<{ key: string; letters: string }> = [
  { key: '1', letters: '' }, { key: '2', letters: 'ABC' }, { key: '3', letters: 'DEF' },
  { key: '4', letters: 'GHI' }, { key: '5', letters: 'JKL' }, { key: '6', letters: 'MNO' },
  { key: '7', letters: 'PQRS' }, { key: '8', letters: 'TUV' }, { key: '9', letters: 'WXYZ' },
  { key: '*', letters: '' }, { key: '0', letters: '+' }, { key: '#', letters: '' },
];

/** The twelve keys of a phone. The caller decides what a key does: dial or send a tone. */
export function DialPad({ onKey, label }: { onKey: (key: string) => void; label: string }) {
  return (
    <div className="dial-pad" role="group" aria-label={label}>
      {KEYS.map(({ key, letters }) => (
        <button key={key} type="button" className="dial-key" aria-label={key} onClick={() => onKey(key)}>
          <span className="dial-digit">{key}</span>
          <span className="dial-letters" aria-hidden="true">{letters}</span>
        </button>
      ))}
    </div>
  );
}
