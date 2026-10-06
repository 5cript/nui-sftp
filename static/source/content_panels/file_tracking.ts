import { NuiWidget } from './nui_widget';

class FileTracking extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'file-tracking', 'fileTracking');
        this.title.closable = true;
    }
}

export { FileTracking };
