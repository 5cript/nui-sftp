import { NuiWidget } from './nui_widget';
import { AttentionPulse, tabBadgeKeys } from './badge_tab_renderer';
import { Message } from '@lumino/messaging';

/** What the File Tracking dock tab shows, as DockTabAttention sends it through ContentPanelManager. */
interface FileTrackingAttention {
    /** Conflicts waiting in the session, the yellow count. */
    conflicts: number;
    /** Failed uploads of the session, the red count. */
    failed: number;
    /** Play the glow now. */
    pulse: boolean;
    /** Translated title of the yellow count, empty at 0. */
    conflictsText: string;
    /** Translated title of the red count, empty at 0. */
    failedText: string;
}

/** How long a pulse keeps data-attention-pulse on the tab: three glows of 0.83 s and some slack. */
const attentionPulseMilliseconds = 2600;

class FileTracking extends NuiWidget {
    private readonly activated: (() => any) | undefined;
    private attention: FileTrackingAttention | undefined = undefined;
    /** Alternates on every pulse, so a pulse right after the previous one still restarts the keyframes. */
    private pulsePhase: AttentionPulse = 'b';
    /** Pending while a pulse plays. */
    private pulseTimer: ReturnType<typeof setTimeout> | undefined = undefined;

    constructor(
        name: string,
        factory: () => HTMLElement | undefined,
        deleter: () => any,
        activated?: () => any
    ) {
        super(name, factory, deleter, 'file-tracking', 'fileTracking');
        this.activated = activated;
        this.title.closable = true;
        this.title.className = 'file-tracking-widget';
    }

    /** Shows the counts on the tab and plays the glow when attention.pulse is set. */
    applyAttention(attention: FileTrackingAttention) {
        this.attention = attention;
        if (attention.pulse) {
            this.pulsePhase = this.pulsePhase === 'a' ? 'b' : 'a';
            clearTimeout(this.pulseTimer);
            this.pulseTimer = setTimeout(() => {
                this.pulseTimer = undefined;
                this.updateTitleDataset();
            }, attentionPulseMilliseconds);
        }
        this.updateTitleDataset();
    }

    dispose(): void {
        clearTimeout(this.pulseTimer);
        this.pulseTimer = undefined;
        super.dispose();
    }

    protected onActivateRequest(message: Message): void {
        super.onActivateRequest(message);
        this.activated?.();
    }

    /** Lumino re-renders the tab only when the title changes, so this always assigns a new dataset object. */
    private updateTitleDataset() {
        const attention = this.attention;
        if (attention === undefined)
            return;
        const dataset: Record<string, string> = {
            [tabBadgeKeys.warning]: String(attention.conflicts),
            [tabBadgeKeys.warningTitle]: attention.conflictsText ?? '',
            [tabBadgeKeys.danger]: String(attention.failed),
            [tabBadgeKeys.dangerTitle]: attention.failedText ?? '',
        };
        if (this.pulseTimer !== undefined)
            dataset[tabBadgeKeys.pulse] = this.pulsePhase;
        this.title.dataset = dataset;
    }
}

export { FileTracking };
export type { FileTrackingAttention };
