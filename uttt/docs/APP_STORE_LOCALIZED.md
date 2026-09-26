# Ultimate Tic-Tac-Toe Messages - localized App Store listing

The store listing (app id 6815039449, version 1.0, not yet submitted) is now localized into all 25 languages the app itself speaks (`shared/c/i18n/languages.h`), with English (en-US) staying the primary locale.
Each locale's name, subtitle, description, keywords and promotional text was written fresh, reusing the terms the app's own `uttt/c/i18n/strings_<code>.c` uses for board, subgrid, square, move and the local name for tic-tac-toe, so the store page and the game speak the same language.
Support URL, marketing URL and privacy policy URL are the same three pages for every locale: `https://uttt.live/support-msg`, `https://uttt.live/about`, `https://uttt.live/privacy-msg`.

Locale choices: `zh` maps to `zh-Hans` (the app's own `strings_zh.c` is Simplified Chinese).
`pt` has no generic ASC locale, so it was split into both `pt-BR` (Brazilian, matching the app's own `strings_pt.c`: "jogo da velha") and `pt-PT` (European: "jogo do galo", "ecrã"-style vocabulary), doubling reach for that one language.
`es` maps to `es-ES`, `fr` to `fr-FR`, `de` to `de-DE`, `nl` to `nl-NL` and `ar` to `ar-SA`, each the single ASC region variant used since the app itself carries one `es`/`fr`/`de`/`nl`/`ar` file.
`no` (Norwegian) has a plain ASC locale, matching the app's single `no`.
foolish's own listing (app 6792255464) was checked for a locale set to mirror, but it turned out to carry only `en-US` on both its `appInfoLocalizations` and `appStoreVersionLocalizations` (live 1.1 and in-review 1.2 versions alike), so there was no existing locale set or code choice to copy; the mapping above was decided fresh from ASC's supported locale list.

"Ultimate Tic-Tac-Toe" was kept recognizable per market rather than forced into one fixed spelling: kept in Latin as `Ultimate Tic-Tac-Toe` for German, Indonesian and (paired with the local game name) Dutch; combined with the app's own local game term for Russian, Ukrainian, Vietnamese, Spanish, both Portuguese variants, French, Italian, Japanese, Polish, Turkish, Thai, Swedish, Danish, Norwegian, Finnish, Czech, Romanian, Hebrew and Arabic, since those are the names the app's own rulebook already uses for tic-tac-toe (кресты-нолики, jogo da velha/do galo, morpion, tris, 三目並べ, XOX, OX, tre i rad, kryds og bolle, bondesjakk, ristinolla, piškvorky, X și 0, איקס עיגול, إكس أو); and translated outright for Korean and Simplified Chinese (얼티밋 틱택토, 终极井字棋), which read naturally as full local names.
Three app names collided with an app already on the Store under a different developer account (Russian, Chinese and Spanish default names) and were resolved by rephrasing as `Ultimate: <local name>` instead, which ASC accepted.

## Locale table

| Locale | Name | Subtitle | Description opens with |
|---|---|---|---|
| en-US (primary) | Ultimate Tic-Tac-Toe Messages | Tic-Tac-Toe with a twist | Ultimate Tic-Tac-Toe, played with a friend right in Messages. |
| ru | Ultimate: Крестики-нолики | Крестики-нолики с изюминкой | Крестики-нолики Ultimate: играйте с другом прямо в Сообщениях. |
| ko | 얼티밋 틱택토 | 반전 있는 틱택토 게임 | 메시지 안에서 친구와 바로 즐기는 얼티밋 틱택토. |
| zh-Hans | 终极井字棋 Ultimate | 反转玩法的井字棋 | 终极井字棋，直接在信息中和朋友对战。 |
| vi | Cờ Ca-rô Ultimate | Cờ ca-rô kiểu mới lạ | Cờ Ca-rô Ultimate, chơi cùng bạn bè ngay trong Messages. |
| es-ES | Ultimate: Tres en Raya | Tres en raya con un giro | Tres en Raya Ultimate, para jugar con un amigo directamente en Mensajes. |
| pt-BR | Jogo da Velha Ultimate | Jogo da velha com uma virada | Jogo da Velha Ultimate, para jogar com um amigo direto no Mensagens. |
| pt-PT | Jogo do Galo Ultimate | Jogo do galo com um truque | Jogo do Galo Ultimate, para jogar com um amigo diretamente no Mensagens. |
| fr-FR | Morpion Ultimate | Le morpion version corsée | Morpion Ultimate, à jouer avec un ami directement dans Messages. |
| de-DE | Ultimate Tic-Tac-Toe | Tic-Tac-Toe mit Dreh | Ultimate Tic-Tac-Toe, gespielt mit einem Freund direkt in Nachrichten. |
| it | Tris Ultimate | Il tris con una svolta | Tris Ultimate, da giocare con un amico direttamente in Messaggi. |
| ja | アルティメット三目並べ | ひとひねりある三目並べ | アルティメット三目並べを、メッセージの中で友だちと直接対戦。 |
| pl | Ultimate Kółko i Krzyżyk | Kółko i krzyżyk z twistem | Ultimate Kółko i Krzyżyk, do gry ze znajomym prosto w Wiadomościach. |
| uk | Хрестики-нулики Ultimate | Хрестики-нулики з родзинкою | Хрестики-нулики Ultimate, грайте з другом прямо в Повідомленнях. |
| tr | Ultimate XOX Oyunu | Farkı olan XOX oyunu | Ultimate XOX, arkadaşınla doğrudan Mesajlar'da oyna. |
| id | Ultimate Tic-Tac-Toe | Tic-tac-toe dengan kejutan | Ultimate Tic-Tac-Toe, main bareng teman langsung di Pesan. |
| th | Ultimate OX | เกม OX สุดพลิกผัน | Ultimate OX เล่นกับเพื่อนได้เลยใน Messages |
| nl-NL | Ultimate Boter-Kaas-Eieren | Met een verrassende twist | Ultimate Boter-Kaas-Eieren, speel met een vriend rechtstreeks in Berichten. |
| sv | Tre i Rad Ultimate | Tre i rad med en twist | Tre i Rad Ultimate, spela med en vän direkt i Meddelanden. |
| da | Kryds og Bolle Ultimate | Kryds og bolle med et twist | Kryds og Bolle Ultimate, spil med en ven direkte i Beskeder. |
| no | Bondesjakk Ultimate | Bondesjakk med en vri | Bondesjakk Ultimate, spill med en venn direkte i Meldinger. |
| fi | Ristinolla Ultimate | Ristinolla uudella twistillä | Ristinolla Ultimate, pelaa kaverin kanssa suoraan Viesteissä. |
| cs | Piškvorky Ultimate | Piškvorky s twistem navíc | Piškvorky Ultimate, hraj s kamarádem přímo ve zprávách. |
| ro | Ultimate X și 0 | X și 0 cu o întorsătură | Ultimate X și 0, joacă cu un prieten direct în Mesaje. |
| he | Ultimate איקס עיגול | איקס עיגול עם תפנית | Ultimate איקס עיגול, לשחק עם חבר ישירות בהודעות. |
| ar-SA | Ultimate إكس أو | إكס أو بلمسة مختلفة | Ultimate إكס أو، العب مع صديق مباشرة في الرسائل. |

## What is still open

Screenshots were not touched (they come later, per instructions) and every locale's `appScreenshotSets` is still empty; the listing cannot go to review until at least the primary locale has screenshots.
Nothing was submitted and no build was touched.
The three renamed locales (ru, es-ES, zh-Hans) carry a name of the form `Ultimate: <local name>` instead of `<local name> Ultimate` because the plain form was already taken by another developer's app; the owner may want to try Apple's trademark claim process for the original phrasing, or just keep the colon form, which reads fine.
