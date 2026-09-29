import { useEffect, useState } from 'react';

export type ViewportBox = {
  height: number;
  offsetTop: number;
};

function readViewport(): ViewportBox {
  const viewport = window.visualViewport;
  if (!viewport) return { height: window.innerHeight, offsetTop: 0 };
  return { height: viewport.height, offsetTop: viewport.offsetTop };
}

export function useVisualViewport(): ViewportBox {
  const [box, setBox] = useState(readViewport);

  useEffect(() => {
    const viewport = window.visualViewport;
    const sync = () => setBox(readViewport());
    sync();
    if (!viewport) {
      window.addEventListener('resize', sync);
      return () => window.removeEventListener('resize', sync);
    }
    viewport.addEventListener('resize', sync);
    viewport.addEventListener('scroll', sync);
    return () => {
      viewport.removeEventListener('resize', sync);
      viewport.removeEventListener('scroll', sync);
    };
  }, []);

  return box;
}
