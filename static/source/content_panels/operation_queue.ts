import { Message } from '@lumino/messaging';
import {
    NuiWidget
} from './nui_widget';

class OperationQueue extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'operation-queue', 'operationQueue');

        this.title.closable = true;
    }
}

export { OperationQueue };