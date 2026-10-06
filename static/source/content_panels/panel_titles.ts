export type PanelTitleKey =
    | 'terminal'
    | 'fileExplorer'
    | 'fileTracking'
    | 'operationQueue'
    | 'sessionOptions'
    | 'commandHistory'
    | 'commandSnippets';

/// Tab titles of the panels. English until the C++ side passes translations via
/// ContentPanelManager.setPanelTitles.
export const panelTitles: Record<PanelTitleKey, string> = {
    terminal: 'Terminal',
    fileExplorer: 'File Explorer',
    fileTracking: 'File Tracking',
    operationQueue: 'Operation Queue',
    sessionOptions: 'Session Options',
    commandHistory: 'Command History',
    commandSnippets: 'Command Snippets',
};
