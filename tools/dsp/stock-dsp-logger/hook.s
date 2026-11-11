    .syntax unified
    .thumb
    .thumb_func
    .global hook
hook:
    push {r4-r6}
    ldr  r4, =0x10010000
    ldr  r5, [r4]
    cmp  r5, #4
    bhs  1f
    movs r5, #4
1:
    cmp  r5, #0x10000
    bhi  2f
    adds r6, r4, r5
    ldrh r1, [r0, #0]
    strh r1, [r6, #0]
    ldrh r1, [r0, #2]
    strh r1, [r6, #2]
    ldrh r1, [r0, #4]
    strh r1, [r6, #4]
    ldrh r1, [r0, #6]
    strh r1, [r6, #6]
    ldrh r1, [r0, #8]
    strh r1, [r6, #8]
    adds r5, r5, #10
    str  r5, [r4]
2:
    pop  {r4-r6}
    push {r3-r7, lr}
    movs r4, r0
    b.w  0x0005f478
    .ltorg
