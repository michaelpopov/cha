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

export function SidebarIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <rect height="18" rx="2" width="18" x="3" y="3" {...stroke} />
      <path d="M9 3v18" {...stroke} />
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

export function SettingsIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <circle cx="12" cy="12" r="3" {...stroke} />
      <path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M18.7 5.3l-1.4 1.4M6.7 17.3l-1.4 1.4" {...stroke} />
      <circle cx="12" cy="12" r="7" {...stroke} />
    </Icon>
  );
}

export function KeyIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <circle cx="8" cy="12" r="4" {...stroke} />
      <path d="M12 12h9M17 12v3M20 12v2" {...stroke} />
    </Icon>
  );
}

export function DatabaseIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <ellipse cx="12" cy="5" rx="7" ry="3" {...stroke} />
      <path d="M5 5v7c0 1.7 3.1 3 7 3s7-1.3 7-3V5M5 12v7c0 1.7 3.1 3 7 3s7-1.3 7-3v-7" {...stroke} />
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

export function MicrophoneIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <rect height="11" rx="4" width="7" x="8.5" y="3" {...stroke} />
      <path d="M5.5 11.5a6.5 6.5 0 0 0 13 0M12 18v3M8.5 21h7" {...stroke} />
    </Icon>
  );
}

export function SpeakerIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M5 10h3l4-3.5v11L8 14H5zM16 9a4.5 4.5 0 0 1 0 6M18.5 6.5a8 8 0 0 1 0 11" {...stroke} />
    </Icon>
  );
}

export function EyeIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6Z" {...stroke} />
      <circle cx="12" cy="12" r="2.5" {...stroke} />
    </Icon>
  );
}

export function EyeOffIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6Z" {...stroke} />
      <circle cx="12" cy="12" r="2.5" {...stroke} />
      <path d="M3 3l18 18" {...stroke} />
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

export function MoreIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <circle cx="6" cy="12" fill="currentColor" r="1.4" />
      <circle cx="12" cy="12" fill="currentColor" r="1.4" />
      <circle cx="18" cy="12" fill="currentColor" r="1.4" />
    </Icon>
  );
}

export function EditIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="m4 20 4.5-1 10-10a2.1 2.1 0 0 0-3-3l-10 10L4 20Z" {...stroke} />
      <path d="m14 7 3 3" {...stroke} />
    </Icon>
  );
}

export function CheckIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="m5 12 4 4L19 6" {...stroke} />
    </Icon>
  );
}

export function CloseIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="m6 6 12 12M18 6 6 18" {...stroke} />
    </Icon>
  );
}

export function FileUpIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9Z" {...stroke} />
      <path d="M14 3v6h6M12 18v-6M9.5 14.5 12 12l2.5 2.5" {...stroke} />
    </Icon>
  );
}

export function TextLinesIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M5 6h14M5 10h14M5 14h10M5 18h12" {...stroke} />
    </Icon>
  );
}

export function SkullBonesIcon(props: IconProps) {
  return (
    <Icon {...props}>
      <path d="M8.2 14.5 4 18.7M15.8 14.5l4.2 4.2M5.3 14l4.7 4.7M18.7 14 14 18.7" {...stroke} />
      <path d="M7 10.5V9a5 5 0 0 1 10 0v1.5c0 1.8-.9 3-2.4 3.7V17H9.4v-2.8C7.9 13.5 7 12.3 7 10.5Z" fill="var(--background)" {...stroke} />
      <circle cx="10" cy="10" r="1" fill="currentColor" />
      <circle cx="14" cy="10" r="1" fill="currentColor" />
      <path d="M12 12.2v1.2M11 17v-1.4M13 17v-1.4" {...stroke} />
    </Icon>
  );
}
