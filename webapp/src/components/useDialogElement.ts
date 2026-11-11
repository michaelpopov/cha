import { useEffect, useRef } from 'react';

export function useDialogElement(focus: () => void) {
  const element = useRef<HTMLDialogElement | null>(null);
  const previousFocus = useRef<HTMLElement | null>(null);
  const focusRef = useRef(focus);
  focusRef.current = focus;
  useEffect(() => {
    previousFocus.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const node = element.current;
    if (!node) return;
    if (typeof node.showModal === 'function') node.showModal();
    else node.setAttribute('open', '');
    focusRef.current();
    return () => {
      if (typeof node.close === 'function') node.close();
      if (previousFocus.current?.isConnected) previousFocus.current.focus();
    };
  }, []);
  return element;
}
