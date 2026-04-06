import { NuiWidget } from './nui_widget';

class FileTracking extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'file-tracking');
        this.title.label = name;
        this.title.closable = true;
        this.title.caption = 'File Tracking';
    }
}

export { FileTracking };
