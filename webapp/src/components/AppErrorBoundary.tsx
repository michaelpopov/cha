import { Component, type ErrorInfo, type ReactNode } from 'react';

interface AppErrorBoundaryProps {
  children: ReactNode;
  onReload(): void;
}

interface AppErrorBoundaryState {
  failed: boolean;
}

export class AppErrorBoundary extends Component<
  AppErrorBoundaryProps,
  AppErrorBoundaryState
> {
  state: AppErrorBoundaryState = { failed: false };

  static getDerivedStateFromError(): AppErrorBoundaryState {
    return { failed: true };
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('CHA render failed.', error, info);
  }

  render() {
    if (!this.state.failed) return this.props.children;
    return (
      <div className="cha-state-message cha-error-message" role="alert">
        <p>Something went wrong while showing CHA.</p>
        <button
          className="cha-button cha-button-ghost"
          onClick={this.props.onReload}
          type="button"
        >
          Reload CHA
        </button>
      </div>
    );
  }
}
