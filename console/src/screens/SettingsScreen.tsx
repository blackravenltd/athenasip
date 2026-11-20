export function SettingsScreen() {
  return (
    <>
      <h1>Settings</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Server configuration</h2>
            <p>
              AthenaSIP reads its configuration from <code>~/.athenasip/config.yaml</code> at
              startup: SIP timers, transports, TLS, the datastore, the event system and the
              media engine.
            </p>
          </div>
        </div>
        <p className="field-hint">
          Editing it from here needs read and write endpoints the server does not expose yet,
          and a clear distinction between what can change on a running node and what needs a
          restart. Until then this screen would only be able to lie about which it was doing.
        </p>
      </section>
    </>
  );
}
