# Cancel behavior

What a cancel in the operation queue does with the partial file of a transfer and with its card.

| Code | Partial file | Card |
|---|---|---|
| KS | kept | stays in the queue, can continue |
| KR | kept | removed from the queue |
| ES | deleted | stays in the queue |
| ER | deleted | removed from the queue |

No setting changes what a cancel does, the choice is made per cancel. The menu next to Cancel All shows once a
transfer that can keep its partial file was queued.

An archive transfer never keeps its partial archive, nothing continues it. Every cancel of one gives ES.

A transfer that fails is no cancel: a failed upload keeps its partial file, a failed download deletes it unless
"Delete Partial Files on Failure" is off in the download options.

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

A card whose kept partial file is deleted (ER from KS) goes once the file is gone. When the file cannot be deleted
the card stays, to try again.

Without a connected session nothing runs, so all three take every card away, the running and queued ones too. Kept
partial files stay (KR). "Cancel all and delete partial files" deletes the local ones (ER). A card that kept a partial
file on the server stays until a session is connected, the file cannot be deleted before.

With "Auto Remove Completed operations" on, a card without kept partial files (ES) goes by itself 5 seconds after it
ended. A card that kept its partial file (KS) stays until it is continued or removed.

A partial file that was kept without its card (KR) is continued by starting the same transfer again with "Try to
Continue" on.
