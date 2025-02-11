; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -passes=loop-fusion < %s | FileCheck %s

@B = common global [1024 x i32] zeroinitializer, align 16

define void @dep_free_parametric(i32* noalias %A, i64 %N) {
; CHECK-LABEL: @dep_free_parametric(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[CMP4:%.*]] = icmp slt i64 0, [[N:%.*]]
; CHECK-NEXT:    [[CMP31:%.*]] = icmp slt i64 0, [[N]]
; CHECK-NEXT:    br i1 [[CMP4]], label [[BB3:%.*]], label [[BB12:%.*]]
; CHECK:       bb3:
; CHECK-NEXT:    br label [[BB5:%.*]]
; CHECK:       bb5:
; CHECK-NEXT:    [[I_05:%.*]] = phi i64 [ [[INC:%.*]], [[BB5]] ], [ 0, [[BB3]] ]
; CHECK-NEXT:    [[I1_02:%.*]] = phi i64 [ [[INC14:%.*]], [[BB5]] ], [ 0, [[BB3]] ]
; CHECK-NEXT:    [[SUB:%.*]] = sub nsw i64 [[I_05]], 3
; CHECK-NEXT:    [[ADD:%.*]] = add nsw i64 [[I_05]], 3
; CHECK-NEXT:    [[MUL:%.*]] = mul nsw i64 [[SUB]], [[ADD]]
; CHECK-NEXT:    [[REM:%.*]] = srem i64 [[MUL]], [[I_05]]
; CHECK-NEXT:    [[CONV:%.*]] = trunc i64 [[REM]] to i32
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i32, i32* [[A:%.*]], i64 [[I_05]]
; CHECK-NEXT:    store i32 [[CONV]], i32* [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[INC]] = add nsw i64 [[I_05]], 1
; CHECK-NEXT:    [[CMP:%.*]] = icmp slt i64 [[INC]], [[N]]
; CHECK-NEXT:    [[SUB7:%.*]] = sub nsw i64 [[I1_02]], 3
; CHECK-NEXT:    [[ADD8:%.*]] = add nsw i64 [[I1_02]], 3
; CHECK-NEXT:    [[MUL9:%.*]] = mul nsw i64 [[SUB7]], [[ADD8]]
; CHECK-NEXT:    [[REM10:%.*]] = srem i64 [[MUL9]], [[I1_02]]
; CHECK-NEXT:    [[CONV11:%.*]] = trunc i64 [[REM10]] to i32
; CHECK-NEXT:    [[ARRAYIDX12:%.*]] = getelementptr inbounds [1024 x i32], [1024 x i32]* @B, i64 0, i64 [[I1_02]]
; CHECK-NEXT:    store i32 [[CONV11]], i32* [[ARRAYIDX12]], align 4
; CHECK-NEXT:    [[INC14]] = add nsw i64 [[I1_02]], 1
; CHECK-NEXT:    [[CMP3:%.*]] = icmp slt i64 [[INC14]], [[N]]
; CHECK-NEXT:    br i1 [[CMP3]], label [[BB5]], label [[BB15:%.*]]
; CHECK:       bb15:
; CHECK-NEXT:    br label [[BB12]]
; CHECK:       bb12:
; CHECK-NEXT:    ret void
;
entry:
  %cmp4 = icmp slt i64 0, %N
  br i1 %cmp4, label %bb3, label %bb14

bb3:                               ; preds = %entry
  br label %bb5

bb5:                                         ; preds = %bb3, %bb5
  %i.05 = phi i64 [ %inc, %bb5 ], [ 0, %bb3 ]
  %sub = sub nsw i64 %i.05, 3
  %add = add nsw i64 %i.05, 3
  %mul = mul nsw i64 %sub, %add
  %rem = srem i64 %mul, %i.05
  %conv = trunc i64 %rem to i32
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %i.05
  store i32 %conv, i32* %arrayidx, align 4
  %inc = add nsw i64 %i.05, 1
  %cmp = icmp slt i64 %inc, %N
  br i1 %cmp, label %bb5, label %bb10

bb10:                                 ; preds = %bb5
  br label %bb14

bb14:                                          ; preds = %bb10, %entry
  %cmp31 = icmp slt i64 0, %N
  br i1 %cmp31, label %bb8, label %bb12

bb8:                              ; preds = %bb14
  br label %bb9

bb9:                                        ; preds = %bb8, %bb9
  %i1.02 = phi i64 [ %inc14, %bb9 ], [ 0, %bb8 ]
  %sub7 = sub nsw i64 %i1.02, 3
  %add8 = add nsw i64 %i1.02, 3
  %mul9 = mul nsw i64 %sub7, %add8
  %rem10 = srem i64 %mul9, %i1.02
  %conv11 = trunc i64 %rem10 to i32
  %arrayidx12 = getelementptr inbounds [1024 x i32], [1024 x i32]* @B, i64 0, i64 %i1.02
  store i32 %conv11, i32* %arrayidx12, align 4
  %inc14 = add nsw i64 %i1.02, 1
  %cmp3 = icmp slt i64 %inc14, %N
  br i1 %cmp3, label %bb9, label %bb15

bb15:                               ; preds = %bb9
  br label %bb12

bb12:                                        ; preds = %bb15, %bb14
  ret void
}

; Test that `%add` is moved in for.first.preheader, and the two loops for.first
; and for.second are fused.

define void @moveinsts_preheader(i32* noalias %A, i32* noalias %B, i64 %N, i32 %x) {
; CHECK-LABEL: @moveinsts_preheader(
; CHECK-NEXT:  for.first.guard:
; CHECK-NEXT:    [[CMP_GUARD:%.*]] = icmp slt i64 0, [[N:%.*]]
; CHECK-NEXT:    br i1 [[CMP_GUARD]], label [[FOR_FIRST_PREHEADER:%.*]], label [[FOR_END:%.*]]
; CHECK:       for.first.preheader:
; CHECK-NEXT:    [[ADD:%.*]] = add nsw i32 [[X:%.*]], 1
; CHECK-NEXT:    br label [[FOR_FIRST:%.*]]
; CHECK:       for.first:
; CHECK-NEXT:    [[I:%.*]] = phi i64 [ [[INC_I:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[J:%.*]] = phi i64 [ [[INC_J:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[AI:%.*]] = getelementptr inbounds i32, i32* [[A:%.*]], i64 [[I]]
; CHECK-NEXT:    store i32 0, i32* [[AI]], align 4
; CHECK-NEXT:    [[INC_I]] = add nsw i64 [[I]], 1
; CHECK-NEXT:    [[CMP_I:%.*]] = icmp slt i64 [[INC_I]], [[N]]
; CHECK-NEXT:    [[BJ:%.*]] = getelementptr inbounds i32, i32* [[B:%.*]], i64 [[J]]
; CHECK-NEXT:    store i32 0, i32* [[BJ]], align 4
; CHECK-NEXT:    [[INC_J]] = add nsw i64 [[J]], 1
; CHECK-NEXT:    [[CMP_J:%.*]] = icmp slt i64 [[INC_J]], [[N]]
; CHECK-NEXT:    br i1 [[CMP_J]], label [[FOR_FIRST]], label [[FOR_SECOND_EXIT:%.*]]
; CHECK:       for.second.exit:
; CHECK-NEXT:    br label [[FOR_END]]
; CHECK:       for.end:
; CHECK-NEXT:    ret void
;
for.first.guard:
  %cmp.guard = icmp slt i64 0, %N
  br i1 %cmp.guard, label %for.first.preheader, label %for.second.guard

for.first.preheader:
  br label %for.first

for.first:
  %i = phi i64 [ %inc.i, %for.first ], [ 0, %for.first.preheader ]
  %Ai = getelementptr inbounds i32, i32* %A, i64 %i
  store i32 0, i32* %Ai, align 4
  %inc.i = add nsw i64 %i, 1
  %cmp.i = icmp slt i64 %inc.i, %N
  br i1 %cmp.i, label %for.first, label %for.first.exit

for.first.exit:
  br label %for.second.guard

for.second.guard:
  br i1 %cmp.guard, label %for.second.preheader, label %for.end

for.second.preheader:
  %add = add nsw i32 %x, 1
  br label %for.second

for.second:
  %j = phi i64 [ %inc.j, %for.second ], [ 0, %for.second.preheader ]
  %Bj = getelementptr inbounds i32, i32* %B, i64 %j
  store i32 0, i32* %Bj, align 4
  %inc.j = add nsw i64 %j, 1
  %cmp.j = icmp slt i64 %inc.j, %N
  br i1 %cmp.j, label %for.second, label %for.second.exit

for.second.exit:
  br label %for.end

for.end:
  ret void
}

; Test that `%add` is moved in for.second.exit, and the two loops for.first
; and for.second are fused.

define void @moveinsts_exitblock(i32* noalias %A, i32* noalias %B, i64 %N, i32 %x) {
; CHECK-LABEL: @moveinsts_exitblock(
; CHECK-NEXT:  for.first.guard:
; CHECK-NEXT:    [[CMP_GUARD:%.*]] = icmp slt i64 0, [[N:%.*]]
; CHECK-NEXT:    br i1 [[CMP_GUARD]], label [[FOR_FIRST_PREHEADER:%.*]], label [[FOR_END:%.*]]
; CHECK:       for.first.preheader:
; CHECK-NEXT:    br label [[FOR_FIRST:%.*]]
; CHECK:       for.first:
; CHECK-NEXT:    [[I_04:%.*]] = phi i64 [ [[INC:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[J_02:%.*]] = phi i64 [ [[INC6:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i32, i32* [[A:%.*]], i64 [[I_04]]
; CHECK-NEXT:    store i32 0, i32* [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[INC]] = add nsw i64 [[I_04]], 1
; CHECK-NEXT:    [[CMP:%.*]] = icmp slt i64 [[INC]], [[N]]
; CHECK-NEXT:    [[ARRAYIDX4:%.*]] = getelementptr inbounds i32, i32* [[B:%.*]], i64 [[J_02]]
; CHECK-NEXT:    store i32 0, i32* [[ARRAYIDX4]], align 4
; CHECK-NEXT:    [[INC6]] = add nsw i64 [[J_02]], 1
; CHECK-NEXT:    [[CMP_J:%.*]] = icmp slt i64 [[INC6]], [[N]]
; CHECK-NEXT:    br i1 [[CMP_J]], label [[FOR_FIRST]], label [[FOR_SECOND_EXIT:%.*]]
; CHECK:       for.second.exit:
; CHECK-NEXT:    [[ADD:%.*]] = add nsw i32 [[X:%.*]], 1
; CHECK-NEXT:    br label [[FOR_END]]
; CHECK:       for.end:
; CHECK-NEXT:    ret void
;
for.first.guard:
  %cmp.guard = icmp slt i64 0, %N
  br i1 %cmp.guard, label %for.first.preheader, label %for.second.guard

for.first.preheader:
  br label %for.first

for.first:
  %i.04 = phi i64 [ %inc, %for.first ], [ 0, %for.first.preheader ]
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %i.04
  store i32 0, i32* %arrayidx, align 4
  %inc = add nsw i64 %i.04, 1
  %cmp = icmp slt i64 %inc, %N
  br i1 %cmp, label %for.first, label %for.first.exit

for.first.exit:
  %add = add nsw i32 %x, 1
  br label %for.second.guard

for.second.guard:
  br i1 %cmp.guard, label %for.second.preheader, label %for.end

for.second.preheader:
  br label %for.second

for.second:
  %j.02 = phi i64 [ %inc6, %for.second ], [ 0, %for.second.preheader ]
  %arrayidx4 = getelementptr inbounds i32, i32* %B, i64 %j.02
  store i32 0, i32* %arrayidx4, align 4
  %inc6 = add nsw i64 %j.02, 1
  %cmp.j = icmp slt i64 %inc6, %N
  br i1 %cmp.j, label %for.second, label %for.second.exit

for.second.exit:
  br label %for.end

for.end:
  ret void
}

; Test that `%add` is moved in for.first.guard, and the two loops for.first
; and for.second are fused.

define void @moveinsts_guardblock(i32* noalias %A, i32* noalias %B, i64 %N, i32 %x) {
; CHECK-LABEL: @moveinsts_guardblock(
; CHECK-NEXT:  for.first.guard:
; CHECK-NEXT:    [[CMP_GUARD:%.*]] = icmp slt i64 0, [[N:%.*]]
; CHECK-NEXT:    [[ADD:%.*]] = add nsw i32 [[X:%.*]], 1
; CHECK-NEXT:    br i1 [[CMP_GUARD]], label [[FOR_FIRST_PREHEADER:%.*]], label [[FOR_END:%.*]]
; CHECK:       for.first.preheader:
; CHECK-NEXT:    br label [[FOR_FIRST:%.*]]
; CHECK:       for.first:
; CHECK-NEXT:    [[I_04:%.*]] = phi i64 [ [[INC:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[J_02:%.*]] = phi i64 [ [[INC6:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i32, i32* [[A:%.*]], i64 [[I_04]]
; CHECK-NEXT:    store i32 0, i32* [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[INC]] = add nsw i64 [[I_04]], 1
; CHECK-NEXT:    [[CMP:%.*]] = icmp slt i64 [[INC]], [[N]]
; CHECK-NEXT:    [[ARRAYIDX4:%.*]] = getelementptr inbounds i32, i32* [[B:%.*]], i64 [[J_02]]
; CHECK-NEXT:    store i32 0, i32* [[ARRAYIDX4]], align 4
; CHECK-NEXT:    [[INC6]] = add nsw i64 [[J_02]], 1
; CHECK-NEXT:    [[CMP_J:%.*]] = icmp slt i64 [[INC6]], [[N]]
; CHECK-NEXT:    br i1 [[CMP_J]], label [[FOR_FIRST]], label [[FOR_SECOND_EXIT:%.*]]
; CHECK:       for.second.exit:
; CHECK-NEXT:    br label [[FOR_END]]
; CHECK:       for.end:
; CHECK-NEXT:    ret void
;
for.first.guard:
  %cmp.guard = icmp slt i64 0, %N
  br i1 %cmp.guard, label %for.first.preheader, label %for.second.guard

for.first.preheader:
  br label %for.first

for.first:
  %i.04 = phi i64 [ %inc, %for.first ], [ 0, %for.first.preheader ]
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %i.04
  store i32 0, i32* %arrayidx, align 4
  %inc = add nsw i64 %i.04, 1
  %cmp = icmp slt i64 %inc, %N
  br i1 %cmp, label %for.first, label %for.first.exit

for.first.exit:
  br label %for.second.guard

for.second.guard:
  %add = add nsw i32 %x, 1
  br i1 %cmp.guard, label %for.second.preheader, label %for.end

for.second.preheader:
  br label %for.second

for.second:
  %j.02 = phi i64 [ %inc6, %for.second ], [ 0, %for.second.preheader ]
  %arrayidx4 = getelementptr inbounds i32, i32* %B, i64 %j.02
  store i32 0, i32* %arrayidx4, align 4
  %inc6 = add nsw i64 %j.02, 1
  %cmp.j = icmp slt i64 %inc6, %N
  br i1 %cmp.j, label %for.second, label %for.second.exit

for.second.exit:
  br label %for.end

for.end:
  ret void
}

; Test that the incoming block of `%j.lcssa` is updated correctly
; from for.second.guard to for.first.guard, and the two loops for.first and
; for.second are fused.

define i64 @updatephi_guardnonloopblock(i32* noalias %A, i32* noalias %B, i64 %N, i32 %x) {
; CHECK-LABEL: @updatephi_guardnonloopblock(
; CHECK-NEXT:  for.first.guard:
; CHECK-NEXT:    [[CMP_GUARD:%.*]] = icmp slt i64 0, [[N:%.*]]
; CHECK-NEXT:    br i1 [[CMP_GUARD]], label [[FOR_FIRST_PREHEADER:%.*]], label [[FOR_END:%.*]]
; CHECK:       for.first.preheader:
; CHECK-NEXT:    br label [[FOR_FIRST:%.*]]
; CHECK:       for.first:
; CHECK-NEXT:    [[I_04:%.*]] = phi i64 [ [[INC:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[J_02:%.*]] = phi i64 [ [[INC6:%.*]], [[FOR_FIRST]] ], [ 0, [[FOR_FIRST_PREHEADER]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i32, i32* [[A:%.*]], i64 [[I_04]]
; CHECK-NEXT:    store i32 0, i32* [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[INC]] = add nsw i64 [[I_04]], 1
; CHECK-NEXT:    [[CMP:%.*]] = icmp slt i64 [[INC]], [[N]]
; CHECK-NEXT:    [[ARRAYIDX4:%.*]] = getelementptr inbounds i32, i32* [[B:%.*]], i64 [[J_02]]
; CHECK-NEXT:    store i32 0, i32* [[ARRAYIDX4]], align 4
; CHECK-NEXT:    [[INC6]] = add nsw i64 [[J_02]], 1
; CHECK-NEXT:    [[CMP_J:%.*]] = icmp slt i64 [[INC6]], [[N]]
; CHECK-NEXT:    br i1 [[CMP_J]], label [[FOR_FIRST]], label [[FOR_SECOND_EXIT:%.*]]
; CHECK:       for.second.exit:
; CHECK-NEXT:    br label [[FOR_END]]
; CHECK:       for.end:
; CHECK-NEXT:    [[J_LCSSA:%.*]] = phi i64 [ 0, [[FOR_FIRST_GUARD:%.*]] ], [ [[J_02]], [[FOR_SECOND_EXIT]] ]
; CHECK-NEXT:    ret i64 [[J_LCSSA]]
;
for.first.guard:
  %cmp.guard = icmp slt i64 0, %N
  br i1 %cmp.guard, label %for.first.preheader, label %for.second.guard

for.first.preheader:
  br label %for.first

for.first:
  %i.04 = phi i64 [ %inc, %for.first ], [ 0, %for.first.preheader ]
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %i.04
  store i32 0, i32* %arrayidx, align 4
  %inc = add nsw i64 %i.04, 1
  %cmp = icmp slt i64 %inc, %N
  br i1 %cmp, label %for.first, label %for.first.exit

for.first.exit:
  br label %for.second.guard

for.second.guard:
  br i1 %cmp.guard, label %for.second.preheader, label %for.end

for.second.preheader:
  br label %for.second

for.second:
  %j.02 = phi i64 [ %inc6, %for.second ], [ 0, %for.second.preheader ]
  %arrayidx4 = getelementptr inbounds i32, i32* %B, i64 %j.02
  store i32 0, i32* %arrayidx4, align 4
  %inc6 = add nsw i64 %j.02, 1
  %cmp.j = icmp slt i64 %inc6, %N
  br i1 %cmp.j, label %for.second, label %for.second.exit

for.second.exit:
  br label %for.end

for.end:
  %j.lcssa = phi i64 [ 0, %for.second.guard ], [ %j.02, %for.second.exit ]
  ret i64 %j.lcssa
}

define void @pr59024() {
; CHECK-LABEL: @pr59024(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[FOR_2_PREHEADER:%.*]], label [[FOR_1_PREHEADER:%.*]]
; CHECK:       for.1.preheader:
; CHECK-NEXT:    br label [[FOR_1:%.*]]
; CHECK:       for.1:
; CHECK-NEXT:    br i1 true, label [[FOR_2_PREHEADER_LOOPEXIT:%.*]], label [[FOR_1]]
; CHECK:       for.2.preheader.loopexit:
; CHECK-NEXT:    br label [[FOR_2_PREHEADER]]
; CHECK:       for.2.preheader:
; CHECK-NEXT:    br label [[FOR_2:%.*]]
; CHECK:       for.2:
; CHECK-NEXT:    br i1 true, label [[EXIT:%.*]], label [[FOR_2]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br i1 false, label %for.2, label %for.1

for.1:                                        ; preds = %for.body6, %entry
  br i1 true, label %for.2, label %for.1

for.2:                                       ; preds = %for.cond13, %for.body6, %entry
  br i1 true, label %exit, label %for.2

exit:                                          ; preds = %for.cond13
  ret void
}
