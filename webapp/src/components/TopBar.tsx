export function TopBar({ title }: { title: string | null }) {
  return (
    <header className="cha-topbar">
      <div className="cha-topbar-title">{title && <h1>{title}</h1>}</div>
    </header>
  );
}
