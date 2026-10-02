# Live arrival host trace, iOS 26.3 simulator (iPhone 17e), 2026-10-02

Excerpts of the extension flight log (flight.log) from RIG_ARRIVE builds; see docs/IMESSAGE_LIVE_ARRIVAL_HOST.md (phase 2).
Columns: seconds since the extension process began, event, footprint MB, peak MB, detail.
Host lines: thr = thread, mt = CACurrentMediaTime, wall = Unix time, msg/sel = payload length and FNV-1a of its bytes (older runs: the link tail), sess = MSSession hash / object id, sender = senderParticipantIdentifier prefix, local = localParticipantIdentifier prefix.

## T-R1: unbound drawer (+ menu), door send, Send pressed

    0.03 host willBecomeActive thr=main mt=266117.7080 wall=1790916512.4436 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0
    0.03 host didBecomeActive thr=main mt=266117.7168 wall=1790916512.4524 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0
    0.04 host willTransition thr=main mt=266117.7178 wall=1790916512.4534 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0 to=compact
    0.04 host didTransition thr=main mt=266117.7179 wall=1790916512.4535 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0
    0.24 host willTransition thr=main mt=266117.9218 wall=1790916512.6575 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0 to=compact
    0.97 host didTransition thr=main mt=266118.6519 wall=1790916513.3876 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0
    4.44 host door-request thr=main mt=266122.1216 wall=1790916516.8574 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0 delivery=send gap=0 session=inherit items=move:0:good base=130b:c8b03797
    4.44 rig move:0:good arrives by send, 129b
    4.46 host door-send thr=main mt=266122.1428 wall=1790916516.8785 msg=129b:67d0f849 sess=0715c33b/ea9992 sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0 item=move:0:good bytes=129b:67d0f849
    4.64 host door-send-done thr=main mt=266122.3194 wall=1790916517.0552 msg=129b:67d0f849 sess=0715c33b/ea9992 sender=- local=B638E849 sel=- selsess=- style=compact conv=66ada0 item=move:0:good error=none
    7.03 host didStartSending thr=main mt=266124.7080 wall=1790916519.4438 msg=129b:67d0f849 sess=0715c33b/6667d9 sender=B0487148 local=B638E849 sel=- selsess=- style=compact conv=66ada0 door=true

## T-R3: bound drawer (tapped bubble), 4 seats, bout-closing Good by another seat

    0.02 host willBecomeActive thr=main mt=266192.7965 wall=1790916587.5335 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b
    0.02 host didBecomeActive thr=main mt=266192.8027 wall=1790916587.5396 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b
    0.09 host willTransition thr=main mt=266192.8667 wall=1790916587.6037 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b to=compact
    0.09 host didTransition thr=main mt=266192.8668 wall=1790916587.6037 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b
    0.33 host willTransition thr=main mt=266193.1124 wall=1790916587.8494 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b to=compact
    1.01 host didTransition thr=main mt=266193.7870 wall=1790916588.5241 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b
    4.65 host didStartSending thr=main mt=266197.4257 wall=1790916592.1628 msg=130b:c8b03797 sess=09a893ca/5a6ff3 sender=9483B946 local=B638E849 sel=- selsess=- style=compact conv=f1ce7b door=false
    4.65 send 130b
    4.65 dismiss first bubble of a game - the drawer cannot be bound
    5.47 host willResignActive thr=main mt=266198.2476 wall=1790916592.9847 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b
    5.47 host didResignActive thr=main mt=266198.2477 wall=1790916592.9847 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=f1ce7b
    5.47 end resigned
    6.89 host willBecomeActive thr=main mt=266199.6693 wall=1790916594.4064 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=compact conv=7fd1b2
    6.89 host didBecomeActive thr=main mt=266199.6720 wall=1790916594.4091 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=compact conv=7fd1b2
    6.90 host willTransition thr=main mt=266199.6768 wall=1790916594.4139 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=compact conv=7fd1b2 to=expanded
    6.90 host didTransition thr=main mt=266199.6770 wall=1790916594.4141 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2
    6.92 host willTransition thr=main mt=266199.7025 wall=1790916594.4396 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2 to=expanded
    6.96 host selection-moved thr=main mt=266199.7352 wall=1790916594.4723 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2
    7.14 host didTransition thr=main mt=266199.9185 wall=1790916594.6556 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2
    29.70 host door-request thr=main mt=266222.4754 wall=1790916617.2129 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2 delivery=send gap=0 session=inherit items=move:0:good base=130b:c8b03797
    29.70 rig move:0:good arrives by send, 129b
    29.70 host door-send thr=main mt=266222.4813 wall=1790916617.2188 msg=129b:add449b1 sess=09a893ca/6e3278 sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2 item=move:0:good bytes=129b:add449b1
    30.01 host door-send-done thr=main mt=266222.7875 wall=1790916617.5250 msg=129b:add449b1 sess=09a893ca/6e3278 sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2 item=move:0:good error=none
    31.68 host willTransition thr=main mt=266224.4559 wall=1790916619.1935 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=expanded conv=7fd1b2 to=compact
    31.68 host didTransition thr=main mt=266224.4562 wall=1790916619.1937 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=compact conv=7fd1b2
    35.28 host willSelect thr=main mt=266228.0536 wall=1790916622.7912 msg=129b:add449b1 sess=09a893ca/3a5ddc sender=0250350A local=B638E849 sel=130b:c8b03797 selsess=09a893ca/6e3278 style=compact conv=7fd1b2
    35.28 host didSelect thr=main mt=266228.0541 wall=1790916622.7917 msg=129b:add449b1 sess=09a893ca/3a5ddc sender=0250350A local=B638E849 sel=129b:add449b1 selsess=09a893ca/3a5ddc style=compact conv=7fd1b2 fresh=false
    35.28 select 129b - routing as an arrival
    35.28 host didReceive thr=main mt=266228.0555 wall=1790916622.7931 msg=129b:add449b1 sess=09a893ca/3a5ddc sender=0250350A local=B638E849 sel=129b:add449b1 selsess=09a893ca/3a5ddc style=compact conv=7fd1b2 door=true
    35.28 receive 48.5 ^56.9
    35.30 reload-is-arrival 129b
    35.30 arrival-ignored already taken
    35.30 host selection-moved thr=main mt=266228.0759 wall=1790916622.8135 msg=- sess=- sender=- local=B638E849 sel=129b:add449b1 selsess=09a893ca/3a5ddc style=compact conv=7fd1b2
    35.30 arrival beats=0 showing=yes phase=2 joins=4
    35.30 arrival-done board
    35.33 anim-open n=2 from=66 seats=-1 kinds=magi,card
    36.33 host didStartSending thr=main mt=266229.1065 wall=1790916623.8441 msg=129b:add449b1 sess=09a893ca/e4fac6 sender=6FF03003 local=B638E849 sel=129b:add449b1 selsess=09a893ca/3a5ddc style=compact conv=7fd1b2 door=true

## T-R2: bound lobby, join x3 then Start through real Messages

    26.36 host door-send-done thr=main mt=265675.8178 wall=1790916070.5455 msg=83b:1b3e98ad sess=01103f0e/7a4cfb sender=- local=1F999618 sel=77b:564d11a6 selsess=01103f0e/7a4cfb style=compact conv=f4ab22 item=join error=none
    27.81 host willSelect thr=main mt=265677.2645 wall=1790916071.9923 msg=83b:1b3e98ad sess=01103f0e/199e0c sender=2CD2C682 local=1F999618 sel=77b:564d11a6 selsess=01103f0e/7a4cfb style=compact conv=f4ab22
    27.81 host didSelect thr=main mt=265677.2649 wall=1790916071.9927 msg=83b:1b3e98ad sess=01103f0e/199e0c sender=2CD2C682 local=1F999618 sel=83b:1b3e98ad selsess=01103f0e/199e0c style=compact conv=f4ab22 fresh=false
    27.81 select 83b - routing as an arrival
    27.81 host didReceive thr=main mt=265677.2659 wall=1790916071.9937 msg=83b:1b3e98ad sess=01103f0e/199e0c sender=2CD2C682 local=1F999618 sel=83b:1b3e98ad selsess=01103f0e/199e0c style=compact conv=f4ab22 door=true
    27.81 receive 39.3 ^48.3
    27.81 host selection-moved thr=main mt=265677.2668 wall=1790916071.9946 msg=- sess=- sender=- local=1F999618 sel=83b:1b3e98ad selsess=01103f0e/199e0c style=compact conv=f4ab22
    27.81 reload-is-arrival 83b
    27.81 arrival-ignored already taken
    27.81 arrival beats=0 showing=yes phase=0 joins=3
    27.81 arrival-done lobby(joins=3 phase=0 game=17011080126887922256)
    28.85 host didStartSending thr=main mt=265678.3014 wall=1790916073.0292 msg=83b:1b3e98ad sess=01103f0e/89e11e sender=96D971C9 local=1F999618 sel=83b:1b3e98ad selsess=01103f0e/199e0c style=compact conv=f4ab22 door=true
    28.89 rig join arrives by send, 88b
    28.89 host door-send thr=main mt=265678.3508 wall=1790916073.0786 msg=88b:cb331e4c sess=01103f0e/199e0c sender=- local=1F999618 sel=83b:1b3e98ad selsess=01103f0e/199e0c style=compact conv=f4ab22 item=join bytes=88b:cb331e4c
    29.05 host door-send-done thr=main mt=265678.5049 wall=1790916073.2327 msg=88b:cb331e4c sess=01103f0e/199e0c sender=- local=1F999618 sel=83b:1b3e98ad selsess=01103f0e/199e0c style=compact conv=f4ab22 item=join error=none
    30.45 host willSelect thr=main mt=265679.9089 wall=1790916074.6368 msg=88b:cb331e4c sess=01103f0e/58f8df sender=2CD2C682 local=1F999618 sel=83b:1b3e98ad selsess=01103f0e/199e0c style=compact conv=f4ab22
    30.45 host didSelect thr=main mt=265679.9093 wall=1790916074.6371 msg=88b:cb331e4c sess=01103f0e/58f8df sender=2CD2C682 local=1F999618 sel=88b:cb331e4c selsess=01103f0e/58f8df style=compact conv=f4ab22 fresh=false
    30.45 select 88b - routing as an arrival
    30.45 host didReceive thr=main mt=265679.9101 wall=1790916074.6380 msg=88b:cb331e4c sess=01103f0e/58f8df sender=2CD2C682 local=1F999618 sel=88b:cb331e4c selsess=01103f0e/58f8df style=compact conv=f4ab22 door=true
    30.45 receive 39.4 ^48.3
    30.46 reload-is-arrival 88b
    30.46 arrival-ignored already taken
    30.46 arrival beats=0 showing=yes phase=0 joins=4
    30.46 arrival-done lobby(joins=4 phase=0 game=17011080126887922256)
    30.46 host selection-moved thr=main mt=265679.9180 wall=1790916074.6458 msg=- sess=- sender=- local=1F999618 sel=88b:cb331e4c selsess=01103f0e/58f8df style=compact conv=f4ab22
    31.51 host didStartSending thr=main mt=265680.9617 wall=1790916075.6896 msg=88b:cb331e4c sess=01103f0e/f0f11d sender=1ADEAE91 local=1F999618 sel=88b:cb331e4c selsess=01103f0e/58f8df style=compact conv=f4ab22 door=true
    31.52 rig start arrives by send, 88b
    31.58 host door-send thr=main mt=265681.0316 wall=1790916075.7594 msg=88b:297575de sess=01103f0e/58f8df sender=- local=1F999618 sel=88b:cb331e4c selsess=01103f0e/58f8df style=compact conv=f4ab22 item=start bytes=88b:297575de
    32.34 host door-send-done thr=main mt=265681.7920 wall=1790916076.5199 msg=88b:297575de sess=01103f0e/58f8df sender=- local=1F999618 sel=88b:cb331e4c selsess=01103f0e/58f8df style=compact conv=f4ab22 item=start error=none
    33.70 host willSelect thr=main mt=265683.1563 wall=1790916077.8842 msg=88b:297575de sess=01103f0e/ca9cd0 sender=2CD2C682 local=1F999618 sel=88b:cb331e4c selsess=01103f0e/58f8df style=compact conv=f4ab22
    33.70 host didSelect thr=main mt=265683.1567 wall=1790916077.8845 msg=88b:297575de sess=01103f0e/ca9cd0 sender=2CD2C682 local=1F999618 sel=88b:297575de selsess=01103f0e/ca9cd0 style=compact conv=f4ab22 fresh=false
    33.70 select 88b - routing as an arrival
    33.70 host didReceive thr=main mt=265683.1576 wall=1790916077.8855 msg=88b:297575de sess=01103f0e/ca9cd0 sender=2CD2C682 local=1F999618 sel=88b:297575de selsess=01103f0e/ca9cd0 style=compact conv=f4ab22 door=true
    33.70 receive 41.5 ^49.1
    33.71 reload-is-arrival 88b
    33.71 arrival-ignored already taken
    33.71 arrival beats=1 showing=yes phase=2 joins=4
    33.72 host selection-moved thr=main mt=265683.1740 wall=1790916077.9019 msg=- sess=- sender=- local=1F999618 sel=88b:297575de selsess=01103f0e/ca9cd0 style=compact conv=f4ab22
    34.23 arrival-done seatPicker
    34.75 host didStartSending thr=main mt=265684.2087 wall=1790916078.9366 msg=88b:297575de sess=01103f0e/20251e sender=0A0D5F1B local=1F999618 sel=88b:297575de selsess=01103f0e/ca9cd0 style=compact conv=f4ab22 door=true

## T-R5: own Good staged as a draft, then another seat throws in

    5.54 host didResignActive thr=main mt=266976.4986 wall=1790917371.2442 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=a82d46
    5.54 end resigned
    6.97 host willBecomeActive thr=main mt=266977.9229 wall=1790917372.6686 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21
    6.97 host didBecomeActive thr=main mt=266977.9253 wall=1790917372.6710 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21
    6.97 host willTransition thr=main mt=266977.9304 wall=1790917372.6761 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 to=expanded
    6.98 host didTransition thr=main mt=266977.9306 wall=1790917372.6763 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=expanded conv=a8ee21
    7.00 host willTransition thr=main mt=266977.9560 wall=1790917372.7017 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=expanded conv=a8ee21 to=expanded
    7.04 host selection-moved thr=main mt=266977.9995 wall=1790917372.7452 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=expanded conv=a8ee21
    7.21 host didTransition thr=main mt=266978.1686 wall=1790917372.9144 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=expanded conv=a8ee21
    7.23 anim-open n=1 from=-1 seats=0 kinds=cove
    24.33 host willTransition thr=main mt=266995.2798 wall=1790917390.0258 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=expanded conv=a8ee21 to=compact
    24.33 host didTransition thr=main mt=266995.2809 wall=1790917390.0269 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21
    29.77 host door-request thr=main mt=267000.7294 wall=1790917395.4755 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 delivery=send gap=0 session=inherit items=move:0hrowin base=105b:b5647718
    29.79 rig move:0hrowin would not seal: damaged(code: -4)
    29.79 host door-refused thr=main mt=267000.7405 wall=1790917395.4866 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 item=move:0hrowin error=damaged(code: -4)
    32.98 host door-request thr=main mt=267003.9301 wall=1790917398.6763 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 delivery=send gap=0 session=inherit items=move:3hrowin base=105b:b5647718
    32.98 rig move:3hrowin would not seal: damaged(code: -4)
    32.98 host door-refused thr=main mt=267003.9305 wall=1790917398.6767 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 item=move:3hrowin error=damaged(code: -4)
    43.77 host door-request thr=main mt=267014.7296 wall=1790917409.4759 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 delivery=send gap=0 session=inherit items=move:0:throwin base=105b:b5647718
    43.78 rig move:0:throwin would not seal: rejected(reason: -1)
    43.78 host door-refused thr=main mt=267014.7350 wall=1790917409.4813 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 item=move:0:throwin error=rejected(reason: -1)
    46.97 host door-request thr=main mt=267017.9285 wall=1790917412.6748 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 delivery=send gap=0 session=inherit items=move:3:throwin base=105b:b5647718
    46.97 rig move:3:throwin arrives by send, 106b
    46.98 host door-send thr=main mt=267017.9387 wall=1790917412.6850 msg=106b:3e4031aa sess=03b82df0/84d189 sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 item=move:3:throwin bytes=106b:3e4031aa
    47.14 host door-send-done thr=main mt=267018.0986 wall=1790917412.8450 msg=106b:3e4031aa sess=03b82df0/84d189 sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 item=move:3:throwin error=none
    68.19 host door-gave-up thr=main mt=267039.1394 wall=1790917433.8861 msg=- sess=- sender=- local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21 item=move:3:throwin never sent
    71.78 host willSelect thr=main mt=267042.7295 wall=1790917437.4763 msg=106b:3e4031aa sess=03b82df0/55c640 sender=0250350A local=B638E849 sel=105b:b5647718 selsess=03b82df0/84d189 style=compact conv=a8ee21
    71.78 host didSelect thr=main mt=267042.7299 wall=1790917437.4767 msg=106b:3e4031aa sess=03b82df0/55c640 sender=0250350A local=B638E849 sel=106b:3e4031aa selsess=03b82df0/55c640 style=compact conv=a8ee21 fresh=false
    71.78 select 106b - routing as an arrival
    71.78 host didReceive thr=main mt=267042.7311 wall=1790917437.4779 msg=106b:3e4031aa sess=03b82df0/55c640 sender=0250350A local=B638E849 sel=106b:3e4031aa selsess=03b82df0/55c640 style=compact conv=a8ee21 door=true
    71.78 receive 49.4 ^58.4
    71.81 reload-is-arrival 106b
    71.81 arrival-ignored already taken
    71.81 arrival beats=0 showing=yes phase=2 joins=4
    71.81 arrival-done board
    71.81 conflict retract 1 staged; prior=ok
    71.83 host selection-moved thr=main mt=267042.7886 wall=1790917437.5353 msg=- sess=- sender=- local=B638E849 sel=106b:3e4031aa selsess=03b82df0/55c640 style=compact conv=a8ee21
    71.85 field-nothing an arrival left the staged bubble stale (superseded)
    71.85 anim-open n=1 from=2 seats=3 kinds=atta
    72.83 host didStartSending thr=main mt=267043.7878 wall=1790917438.5346 msg=106b:3e4031aa sess=03b82df0/686ee5 sender=A1B101BF local=B638E849 sel=106b:3e4031aa selsess=03b82df0/55c640 style=compact conv=a8ee21 door=true

## T-R6: drawer closed, staged door bubble sent, bubble re-tapped

    0.11 host willTransition thr=main mt=267227.3132 wall=1790917622.0630 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=c22bb2 to=compact
    1.06 host didTransition thr=main mt=267228.2684 wall=1790917623.0183 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=c22bb2
    4.41 host didStartSending thr=main mt=267231.6223 wall=1790917626.3722 msg=130b:c8b03797 sess=049773c4/a03a44 sender=626A3F51 local=B638E849 sel=- selsess=- style=compact conv=c22bb2 door=false
    4.41 send 130b
    4.42 dismiss first bubble of a game - the drawer cannot be bound
    5.14 host willResignActive thr=main mt=267232.3491 wall=1790917627.0990 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=c22bb2
    5.14 host didResignActive thr=main mt=267232.3492 wall=1790917627.0991 msg=- sess=- sender=- local=B638E849 sel=- selsess=- style=compact conv=c22bb2
    5.14 end resigned
    6.67 host willBecomeActive thr=main mt=267233.8780 wall=1790917628.6279 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=compact conv=e9d61
    6.67 host didBecomeActive thr=main mt=267233.8807 wall=1790917628.6306 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=compact conv=e9d61
    6.68 host willTransition thr=main mt=267233.8853 wall=1790917628.6352 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=compact conv=e9d61 to=expanded
    6.68 host didTransition thr=main mt=267233.8855 wall=1790917628.6355 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61
    6.69 host willTransition thr=main mt=267233.8987 wall=1790917628.6487 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61 to=expanded
    6.73 host selection-moved thr=main mt=267233.9404 wall=1790917628.6903 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61
    6.91 host didTransition thr=main mt=267234.1185 wall=1790917628.8684 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61
    21.48 host door-request thr=main mt=267248.6888 wall=1790917643.4390 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61 delivery=send gap=0 session=inherit items=move:0:good base=130b:c8b03797
    21.48 rig move:0:good arrives by send, 129b
    21.49 host door-send thr=main mt=267248.6953 wall=1790917643.4455 msg=129b:2cb84ce7 sess=049773c4/37e008 sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61 item=move:0:good bytes=129b:2cb84ce7
    21.80 host door-send-done thr=main mt=267249.0058 wall=1790917643.7560 msg=129b:2cb84ce7 sess=049773c4/37e008 sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61 item=move:0:good error=none
    26.85 host willTransition thr=main mt=267254.0586 wall=1790917648.8088 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=expanded conv=e9d61 to=compact
    26.85 host didTransition thr=main mt=267254.0589 wall=1790917648.8091 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=compact conv=e9d61
    30.99 host willResignActive thr=main mt=267258.1981 wall=1790917652.9484 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=compact conv=e9d61
    30.99 host didResignActive thr=main mt=267258.1983 wall=1790917652.9487 msg=- sess=- sender=- local=B638E849 sel=130b:c8b03797 selsess=049773c4/37e008 style=compact conv=e9d61
    30.99 end resigned
    31.02 host selection-moved thr=main mt=267258.2327 wall=1790917652.9831 msg=- sess=- sender=- local=- sel=- selsess=- style=compact conv=-
    45.30 host willBecomeActive thr=main mt=267272.5062 wall=1790917667.2567 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/f359e5 style=expanded conv=5828d9
    45.30 host didBecomeActive thr=main mt=267272.5080 wall=1790917667.2586 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/f359e5 style=expanded conv=5828d9
    45.30 host didStartSending thr=main mt=267272.5089 wall=1790917667.2595 msg=129b:2cb84ce7 sess=049773c4/d8d3a9 sender=81B61B33 local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/f359e5 style=expanded conv=5828d9 door=false
    45.30 send 129b
    45.30 host willResignActive thr=main mt=267272.5108 wall=1790917667.2613 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/f359e5 style=compact conv=5828d9
    45.30 host didResignActive thr=main mt=267272.5110 wall=1790917667.2616 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/f359e5 style=compact conv=5828d9
    45.30 end resigned
    50.86 host willBecomeActive thr=main mt=267278.0657 wall=1790917672.8163 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/2725e1 style=expanded conv=85e049
    50.86 host didBecomeActive thr=main mt=267278.0671 wall=1790917672.8177 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/2725e1 style=expanded conv=85e049
    50.86 host willTransition thr=main mt=267278.0722 wall=1790917672.8228 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/2725e1 style=expanded conv=85e049 to=expanded
    50.86 host didTransition thr=main mt=267278.0725 wall=1790917672.8231 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/2725e1 style=expanded conv=85e049
    50.87 host willTransition thr=main mt=267278.0749 wall=1790917672.8256 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/2725e1 style=expanded conv=85e049 to=expanded
    50.95 host selection-moved thr=main mt=267278.1571 wall=1790917672.9078 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/2725e1 style=expanded conv=85e049
    51.40 host didTransition thr=main mt=267278.6026 wall=1790917673.3533 msg=- sess=- sender=- local=B638E849 sel=129b:2cb84ce7 selsess=049773c4/2725e1 style=expanded conv=85e049
    51.40 anim-open n=2 from=66 seats=-1 kinds=magi,card

## T-R7: door bubble sent in a NEW MSSession over a bound drawer

    0.03 host willBecomeActive thr=main mt=267393.6498 wall=1790917788.4024 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd
    0.03 host didBecomeActive thr=main mt=267393.6579 wall=1790917788.4105 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd
    0.03 host willTransition thr=main mt=267393.6586 wall=1790917788.4112 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd to=compact
    0.03 host didTransition thr=main mt=267393.6587 wall=1790917788.4112 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd
    0.12 host willTransition thr=main mt=267393.7442 wall=1790917788.4968 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd to=compact
    0.90 host didTransition thr=main mt=267394.5287 wall=1790917789.2813 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd
    4.47 host didStartSending thr=main mt=267398.0941 wall=1790917792.8467 msg=130b:c8b03797 sess=049ad582/5ebea1 sender=2F691D78 local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd door=false
    4.47 send 130b
    4.47 dismiss first bubble of a game - the drawer cannot be bound
    5.31 host willResignActive thr=main mt=267398.9295 wall=1790917793.6822 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd
    5.31 host didResignActive thr=main mt=267398.9296 wall=1790917793.6822 msg=- sess=- sender=- local=D8B22217 sel=- selsess=- style=compact conv=e7a6fd
    5.31 end resigned
    6.63 host willBecomeActive thr=main mt=267400.2487 wall=1790917795.0014 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=compact conv=977e89
    6.63 host didBecomeActive thr=main mt=267400.2513 wall=1790917795.0039 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=compact conv=977e89
    6.63 host willTransition thr=main mt=267400.2564 wall=1790917795.0090 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=compact conv=977e89 to=expanded
    6.63 host didTransition thr=main mt=267400.2566 wall=1790917795.0093 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89
    6.63 host willTransition thr=main mt=267400.2586 wall=1790917795.0112 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89 to=expanded
    6.70 host selection-moved thr=main mt=267400.3247 wall=1790917795.0773 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89
    6.86 host didTransition thr=main mt=267400.4853 wall=1790917795.2380 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89
    21.43 host door-request thr=main mt=267415.0569 wall=1790917809.8098 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89 delivery=send gap=0 session=new items=move:0:good base=130b:c8b03797
    21.44 rig move:0:good arrives by send, 129b
    21.44 host door-send thr=main mt=267415.0647 wall=1790917809.8176 msg=129b:11fd17b5 sess=00912620/ff7699 sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89 item=move:0:good bytes=129b:11fd17b5
    21.75 host door-send-done thr=main mt=267415.3742 wall=1790917810.1271 msg=129b:11fd17b5 sess=00912620/ff7699 sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89 item=move:0:good error=none
    28.54 host willTransition thr=main mt=267422.1676 wall=1790917816.9206 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=expanded conv=977e89 to=compact
    28.54 host didTransition thr=main mt=267422.1679 wall=1790917816.9209 msg=- sess=- sender=- local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=compact conv=977e89
    34.43 host didStartSending thr=main mt=267428.0489 wall=1790917822.8021 msg=129b:11fd17b5 sess=00912620/470884 sender=C39BE982 local=D8B22217 sel=130b:c8b03797 selsess=049ad582/ee59fe style=compact conv=977e89 door=true
