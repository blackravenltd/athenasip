import { useEffect, useId, useRef, type ReactNode } from 'react';
import { createPortal } from 'react-dom';

interface ModalProps {
  open: boolean;
  title: string;
  children: ReactNode;
  actions: ReactNode;
  onClose: () => void;
}

/**
 * A focus-managed dialogue: focus moves in on open and back on close, Escape
 * cancels, and Tab cycles within the panel.
 *
 * It renders into `document.body`: the main panel is its own stacking context,
 * so a dialogue inside it cannot rise above the sticky navigation.
 */
export function Modal({ open, title, children, actions, onClose }: ModalProps) {
  const titleId = useId();
  const panelRef = useRef<HTMLDivElement>(null);
  const onCloseRef = useRef(onClose);
  onCloseRef.current = onClose;

  useEffect(() => {
    if (!open) return;
    const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : undefined;
    panelRef.current?.querySelector<HTMLElement>('input, select, textarea, button, [href]')?.focus();

    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        onCloseRef.current();
        return;
      }
      if (event.key !== 'Tab' || !panelRef.current) return;
      const focusable = [...panelRef.current.querySelectorAll<HTMLElement>(
        'input:not(:disabled), select:not(:disabled), textarea:not(:disabled), button:not(:disabled), [href]',
      )];
      if (focusable.length === 0) return;
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };

    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('keydown', onKeyDown);
      previousFocus?.focus();
    };
  }, [open]);

  if (!open) return null;
  return createPortal(
    <div
      className="modal-backdrop"
      role="presentation"
      onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}
    >
      <div ref={panelRef} className="modal-panel" role="dialog" aria-modal="true" aria-labelledby={titleId}>
        <h2 id={titleId}>{title}</h2>
        <div className="modal-content">{children}</div>
        <div className="modal-actions">{actions}</div>
      </div>
    </div>,
    document.body,
  );
}

interface FormModalProps {
  open: boolean;
  title: string;
  children: ReactNode;
  submitLabel?: string;
  busy?: boolean;
  submitDisabled?: boolean;
  /** A failure belonging to the dialogue rather than to one field. */
  error?: ReactNode;
  onSubmit: () => void;
  onCancel: () => void;
}

/**
 * A dialogue for editing one record. Lists are read-only; every mutation opens
 * one of these.
 *
 * The commit is wired twice, to the form's `submit` (so Enter in a field
 * commits) and to the button's `click`, because the actions row sits outside
 * the form.
 */
export function FormModal({
  open,
  title,
  children,
  submitLabel = 'Save',
  busy = false,
  submitDisabled = false,
  error,
  onSubmit,
  onCancel,
}: FormModalProps) {
  return (
    <Modal
      open={open}
      title={title}
      onClose={busy ? () => undefined : onCancel}
      actions={<>
        <button className="secondary-button" type="button" disabled={busy} onClick={onCancel}>Cancel</button>
        <button className="primary-button" type="button" disabled={busy || submitDisabled} onClick={onSubmit}>
          {busy ? 'Working...' : submitLabel}
        </button>
      </>}
    >
      <form className="modal-form" onSubmit={(event) => { event.preventDefault(); if (!busy && !submitDisabled) onSubmit(); }}>
        {children}
        {error && <p className="error-message" role="alert">{error}</p>}
      </form>
    </Modal>
  );
}

interface ConfirmModalProps {
  open: boolean;
  title: string;
  children: ReactNode;
  confirmLabel?: string;
  busy?: boolean;
  destructive?: boolean;
  onConfirm: () => void;
  onCancel: () => void;
}

export function ConfirmModal({
  open,
  title,
  children,
  confirmLabel = 'Confirm',
  busy = false,
  destructive = false,
  onConfirm,
  onCancel,
}: ConfirmModalProps) {
  return (
    <Modal
      open={open}
      title={title}
      onClose={busy ? () => undefined : onCancel}
      actions={<>
        <button className="secondary-button" type="button" disabled={busy} onClick={onCancel}>Cancel</button>
        <button
          className={`primary-button${destructive ? ' danger-button' : ''}`}
          type="button"
          disabled={busy}
          onClick={onConfirm}
        >
          {busy ? 'Working...' : confirmLabel}
        </button>
      </>}
    >
      {children}
    </Modal>
  );
}
