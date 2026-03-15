import { Message } from '@lumino/messaging';
import {
    Widget
} from '@lumino/widgets';

class NuiWidget extends Widget {
    deleter: () => any | undefined;
    layoutId: string;
    grabbed: boolean | undefined = undefined;

    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any, layoutId: string) {
        super({ node: factory() });
        this.deleter = deleter;
        this.layoutId = layoutId;

        this.setFlag(Widget.Flag.DisallowLayout);
    }

    protected onAfterDetach(msg: Message): void {
        console.log(`NuiWidget ${this.layoutId} detached, calling deleter`);
        this.deleter();
    }
};

export { NuiWidget };