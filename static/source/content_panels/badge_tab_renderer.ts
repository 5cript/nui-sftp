import {
    DockPanel,
    TabBar,
    Title,
    Widget
} from '@lumino/widgets';
import {
    ElementARIAAttrs,
    ElementBaseAttrs,
    VirtualElement,
    h
} from '@lumino/virtualdom';

/** Phase of a running tab glow; switching between the two restarts the keyframes. */
type AttentionPulse = 'a' | 'b';

/**
 * Keys of Title.dataset that BadgeTabRenderer reads. Lumino also puts the whole dataset onto the tab as data-*
 * attributes, so the tab carries data-badge-warning, data-badge-danger and data-attention-pulse too.
 */
const tabBadgeKeys = {
    warning: 'badge-warning',
    warningTitle: 'badge-warning-title',
    danger: 'badge-danger',
    dangerTitle: 'badge-danger-title',
    pulse: 'attention-pulse',
} as const;

interface TabBadge {
    variant: 'warning' | 'danger';
    countKey: string;
    titleKey: string;
    popsWithPulse: boolean;
}

const tabBadges: readonly TabBadge[] = [
    { variant: 'warning', countKey: tabBadgeKeys.warning, titleKey: tabBadgeKeys.warningTitle, popsWithPulse: true },
    { variant: 'danger', countKey: tabBadgeKeys.danger, titleKey: tabBadgeKeys.dangerTitle, popsWithPulse: false },
];

function badgeCount(dataset: Title.Dataset, badge: TabBadge): number {
    const count = Number(dataset[badge.countKey] ?? 0);
    return Number.isFinite(count) && count > 0 ? count : 0;
}

function shownBadgeTitles(dataset: Title.Dataset): string[] {
    return tabBadges
        .filter(badge => badgeCount(dataset, badge) > 0)
        .map(badge => dataset[badge.titleKey] ?? '')
        .filter(title => title !== '');
}

/** One badge in the 5cript-nui-components Badge markup, hidden at 0 and popping with the tab's pulse. */
function renderBadge(dataset: Title.Dataset, badge: TabBadge): VirtualElement {
    const count = badgeCount(dataset, badge);
    const pulse = dataset[tabBadgeKeys.pulse];
    const badgeDataset: Record<string, string> = { variant: badge.variant };
    if (count === 0)
        badgeDataset.hidden = '';
    if (badge.popsWithPulse && (pulse === 'a' || pulse === 'b'))
        badgeDataset.pop = pulse;
    const attributes = { className: 'script-nui-badge', dataset: badgeDataset };
    const title = dataset[badge.titleKey] ?? '';
    return h.span(title === '' ? attributes : { ...attributes, title }, String(count));
}

/**
 * Tab renderer that shows count badges between the label and the close icon. The counts come from the title's
 * dataset (tabBadgeKeys); a tab without a positive count renders exactly like the default renderer's.
 */
class BadgeTabRenderer extends TabBar.Renderer {
    renderTab(data: TabBar.IRenderData<any>): VirtualElement {
        const key = this.createTabKey(data);
        return h.li(
            {
                id: key,
                key,
                className: this.createTabClass(data),
                title: data.title.caption,
                style: this.createTabStyle(data),
                dataset: this.createTabDataset(data),
                ...this.createTabARIA(data),
            },
            this.renderIcon(data),
            this.renderLabel(data),
            this.renderBadges(data),
            data.title.closable ? this.renderCloseIcon(data) : null
        );
    }

    /** The badge container, or null while no count is above 0. */
    renderBadges(data: TabBar.IRenderData<any>): VirtualElement | null {
        const dataset = data.title.dataset;
        if (tabBadges.every(badge => badgeCount(dataset, badge) === 0))
            return null;
        return h.div(
            { className: 'lm-TabBar-tabBadges', 'aria-hidden': 'true' },
            tabBadges.map(badge => renderBadge(dataset, badge))
        );
    }

    /** Names the tab with its label and the shown badge titles, as the badges themselves are aria-hidden. */
    createTabARIA(data: TabBar.IRenderData<any>): ElementARIAAttrs | ElementBaseAttrs {
        const aria = super.createTabARIA(data);
        const titles = shownBadgeTitles(data.title.dataset);
        if (titles.length === 0)
            return aria;
        return { ...aria, 'aria-label': [data.title.label, ...titles].join(', ') };
    }
}

/** Dock renderer whose tab bars use BadgeTabRenderer. */
class BadgeDockRenderer extends DockPanel.Renderer {
    readonly tabRenderer = new BadgeTabRenderer();

    createTabBar(document?: Document | ShadowRoot): TabBar<Widget> {
        const tabBar = new TabBar<Widget>({ document, renderer: this.tabRenderer });
        tabBar.addClass('lm-DockPanel-tabBar');
        return tabBar;
    }
}

/** Shared like Lumino's default renderers, so tab keys stay stable when a tab moves between tab bars. */
const badgeDockRenderer = new BadgeDockRenderer();

export { BadgeDockRenderer, BadgeTabRenderer, badgeDockRenderer, tabBadgeKeys };
export type { AttentionPulse };
