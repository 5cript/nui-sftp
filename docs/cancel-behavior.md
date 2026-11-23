# Cancel behavior

What a cancel in the operation queue does with the partial file of a transfer and with its card.

| Code | Partial file | Card |
|---|---|---|
| KS | kept | stays in the queue, can continue |
| KR | kept | removed from the queue |
| ES | deleted | stays in the queue |
| ER | deleted | removed from the queue |

The tables assume that "Delete Partial Files" is on in the download and upload options, which is the default. With
it off a plain cancel keeps the partial file (KS) and the menus to choose are not shown.

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

Finished and failed cards are removed by all three, like the cancelled ones. The operations a Cancel All cancels
stay as cancelled cards, a second Cancel All removes them.

A partial file that was kept without its card (KR) is continued by starting the same transfer again with "Try to
Continue" on.
