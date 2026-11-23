# Cancel behavior

What a cancel in the operation queue does with the partial file of a transfer and with its card.

| Code | Partial file | Card |
|---|---|---|
| KS | kept | stays in the queue, can continue |
| KR | kept | removed from the queue |
| ES | deleted | stays in the queue |
| ER | deleted | removed from the queue |

The tables assume that "Delete Partial Files" is on in the download and upload options, which is the default. With
it off a plain cancel and a plain Cancel All keep the partial file (KS), and the card has no menu to choose. The menu
next to Cancel All shows once a transfer with the option on was queued. Its two entries decide for every transfer they
cancel, whatever the option says.

An archive transfer never keeps its partial archive, nothing continues it. Every cancel of one gives ES.

## Single operation

| Action | Result |
|---|---|
| Cancel button | ES |
| Cancel and delete partial files | ES |
| Cancel and keep partial files | KS |
| Trashcan on an ES card | ER |
| Trashcan on a KS card | ER |
| Menu next to that trashcan, "Remove and keep the partial files" | KR |

## Cancel All

| Action | Running or queued | Already cancelled, file deleted | Already cancelled, file kept |
|---|---|---|---|
| Cancel All | ES | ER | KR |
| Cancel all and delete partial files | ES | ER | ER |
| Cancel all and keep partial files | KS | ER | KR |

Finished and failed cards are removed by all three, like the cancelled ones. A failed one stays listed in the Failed
tab. The operations a Cancel All cancels stay as cancelled cards, a second Cancel All removes them.

With "Auto Remove Completed operations" on, a card without kept partial files (ES) goes by itself 5 seconds after it
ended. A card that kept its partial file (KS) stays until it is continued or removed.

A partial file that was kept without its card (KR) is continued by starting the same transfer again with "Try to
Continue" on.
