import { Message } from '@lumino/messaging';
import {
    Widget
} from '@lumino/widgets';
import { PanelTitleKey, panelTitles } from './panel_titles';

class NuiWidget extends Widget {
    deleter: () => any | undefined;
    layoutId: string;
    grabbed: boolean | undefined = undefined;
    /// Which translated title the tab shows; undefined once the tab got a title of its own.
    titleKey: PanelTitleKey | undefined;

    constructor(
        name: string,
        factory: () => HTMLElement | undefined,
        deleter: () => any,
        layoutId: string,
        titleKey?: PanelTitleKey
    ) {
        super({ node: factory() });
        this.deleter = deleter;
        this.layoutId = layoutId;
        this.titleKey = titleKey;
        this.applyTitle();

        this.setFlag(Widget.Flag.DisallowLayout);
    }

    applyTitle() {
        if (this.titleKey === undefined)
            return;
        this.title.label = panelTitles[this.titleKey];
        this.title.caption = panelTitles[this.titleKey];
    }

    protected onAfterDetach(msg: Message): void {
        console.log(`NuiWidget ${this.layoutId} detached, calling deleter`);
        this.deleter();
    }
};

export { NuiWidget };