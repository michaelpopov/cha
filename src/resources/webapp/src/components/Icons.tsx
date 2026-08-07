import type { SVGProps } from 'react';

type IconProps = SVGProps<SVGSVGElement>;

function Icon({ children, ...props }: IconProps) {
  return (
    <svg
      aria-hidden="true"
      fill="none"
      height="20"
      viewBox="0 0 24 24"
      width="20"
      {...props}
    >
      {children}
    </svg>
  );
}

const stroke = {
  stroke: 'currentColor',
  strokeLinecap: 'round' as const,
  strokeLinejoin: 'round' as const,
  strokeWidth: 1.7,
};

export function MenuIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M5 9h14M5 15h10" {...stroke} />
    </Icon>
  );
}

export function PersonasIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <circle cx="9" cy="8" r="3" {...stroke} />
      <path d="M3.5 19c.4-3.1 2.2-5 5.5-5s5.1 1.9 5.5 5M16 5.5a3 3 0 0 1 0 5.8M16.5 14c2.4.3 3.7 2 4 4.5" {...stroke} />
    </Icon>
  );
}

export function CharacterIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <circle cx="12" cy="8" r="3.2" {...stroke} />
      <path d="M5.5 20c.5-4 2.7-6 6.5-6s6 2 6.5 6" {...stroke} />
    </Icon>
  );
}

export function ForumsIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M4 5.5h11v8H9l-4 3v-3H4zM9 17h6l4 3v-3h1V9h-2" {...stroke} />
    </Icon>
  );
}

export function MessageIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M4 5h16v11H9l-5 4z" {...stroke} />
    </Icon>
  );
}

export function ChevronRightIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="m9 5 7 7-7 7" {...stroke} />
    </Icon>
  );
}

export function ChevronLeftIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="m15 5-7 7 7 7" {...stroke} />
    </Icon>
  );
}

export function TargetIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <circle cx="12" cy="12" r="7" {...stroke} />
      <circle cx="12" cy="12" r="2" {...stroke} />
      <path d="M12 3V1M21 12h2M12 21v2M3 12H1" {...stroke} />
    </Icon>
  );
}

export function SendIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="m12 19V5m-6 6 6-6 6 6" {...stroke} />
    </Icon>
  );
}

export function StopIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <rect fill="currentColor" height="9" rx="1" width="9" x="7.5" y="7.5" />
    </Icon>
  );
}

export function PlusIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M12 5v14M5 12h14" {...stroke} />
    </Icon>
  );
}
