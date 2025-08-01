; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -passes=loop-vectorize -mtriple=x86_64-apple-macosx10.8.0 -mcpu=corei7-avx -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.8.0"

define i32 @conversion_cost1(i32 %n, ptr nocapture %A, ptr nocapture %B) nounwind uwtable ssp {
; CHECK-LABEL: @conversion_cost1(
; CHECK-NEXT:    [[TMP1:%.*]] = icmp sgt i32 [[N:%.*]], 3
; CHECK-NEXT:    br i1 [[TMP1]], label [[ITER_CHECK:%.*]], label [[DOT_CRIT_EDGE:%.*]]
; CHECK:       iter.check:
; CHECK-NEXT:    [[TMP2:%.*]] = add i32 [[N]], -3
; CHECK-NEXT:    [[TMP3:%.*]] = zext i32 [[TMP2]] to i64
; CHECK-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 [[TMP3]], 4
; CHECK-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[VEC_EPILOG_SCALAR_PH:%.*]], label [[VECTOR_MAIN_LOOP_ITER_CHECK:%.*]]
; CHECK:       vector.main.loop.iter.check:
; CHECK-NEXT:    [[MIN_ITERS_CHECK1:%.*]] = icmp ult i64 [[TMP3]], 32
; CHECK-NEXT:    br i1 [[MIN_ITERS_CHECK1]], label [[VEC_EPILOG_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    [[N_MOD_VF:%.*]] = urem i64 [[TMP3]], 32
; CHECK-NEXT:    [[N_VEC:%.*]] = sub i64 [[TMP3]], [[N_MOD_VF]]
; CHECK-NEXT:    [[IND_END:%.*]] = add i64 3, [[N_VEC]]
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[VEC_IND:%.*]] = phi <32 x i8> [ <i8 3, i8 4, i8 5, i8 6, i8 7, i8 8, i8 9, i8 10, i8 11, i8 12, i8 13, i8 14, i8 15, i8 16, i8 17, i8 18, i8 19, i8 20, i8 21, i8 22, i8 23, i8 24, i8 25, i8 26, i8 27, i8 28, i8 29, i8 30, i8 31, i8 32, i8 33, i8 34>, [[VECTOR_PH]] ], [ [[VEC_IND_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[OFFSET_IDX:%.*]] = add i64 3, [[INDEX]]
; CHECK-NEXT:    [[TMP5:%.*]] = getelementptr inbounds i8, ptr [[A:%.*]], i64 [[OFFSET_IDX]]
; CHECK-NEXT:    store <32 x i8> [[VEC_IND]], ptr [[TMP5]], align 1
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 32
; CHECK-NEXT:    [[VEC_IND_NEXT]] = add <32 x i8> [[VEC_IND]], splat (i8 32)
; CHECK-NEXT:    [[TMP7:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; CHECK-NEXT:    br i1 [[TMP7]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    [[CMP_N:%.*]] = icmp eq i64 [[TMP3]], [[N_VEC]]
; CHECK-NEXT:    br i1 [[CMP_N]], label [[DOT_CRIT_EDGE_LOOPEXIT:%.*]], label [[VEC_EPILOG_ITER_CHECK:%.*]]
; CHECK:       vec.epilog.iter.check:
; CHECK-NEXT:    [[IND_END5:%.*]] = add i64 3, [[N_VEC]]
; CHECK-NEXT:    [[N_VEC_REMAINING:%.*]] = sub i64 [[TMP3]], [[N_VEC]]
; CHECK-NEXT:    [[MIN_EPILOG_ITERS_CHECK:%.*]] = icmp ult i64 [[N_VEC_REMAINING]], 4
; CHECK-NEXT:    br i1 [[MIN_EPILOG_ITERS_CHECK]], label [[VEC_EPILOG_SCALAR_PH]], label [[VEC_EPILOG_PH]]
; CHECK:       vec.epilog.ph:
; CHECK-NEXT:    [[VEC_EPILOG_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[VEC_EPILOG_ITER_CHECK]] ], [ 0, [[VECTOR_MAIN_LOOP_ITER_CHECK]] ]
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[IND_END]], [[VEC_EPILOG_ITER_CHECK]] ], [ 3, [[VECTOR_MAIN_LOOP_ITER_CHECK]] ]
; CHECK-NEXT:    [[N_MOD_VF2:%.*]] = urem i64 [[TMP3]], 4
; CHECK-NEXT:    [[N_VEC3:%.*]] = sub i64 [[TMP3]], [[N_MOD_VF2]]
; CHECK-NEXT:    [[IND_END4:%.*]] = add i64 3, [[N_VEC3]]
; CHECK-NEXT:    [[TMP8:%.*]] = trunc i64 [[BC_RESUME_VAL]] to i8
; CHECK-NEXT:    [[DOTSPLATINSERT:%.*]] = insertelement <4 x i8> poison, i8 [[TMP8]], i64 0
; CHECK-NEXT:    [[DOTSPLAT:%.*]] = shufflevector <4 x i8> [[DOTSPLATINSERT]], <4 x i8> poison, <4 x i32> zeroinitializer
; CHECK-NEXT:    [[INDUCTION:%.*]] = add <4 x i8> [[DOTSPLAT]], <i8 0, i8 1, i8 2, i8 3>
; CHECK-NEXT:    br label [[VEC_EPILOG_VECTOR_BODY:%.*]]
; CHECK:       vec.epilog.vector.body:
; CHECK-NEXT:    [[INDEX7:%.*]] = phi i64 [ [[VEC_EPILOG_RESUME_VAL]], [[VEC_EPILOG_PH]] ], [ [[INDEX_NEXT11:%.*]], [[VEC_EPILOG_VECTOR_BODY]] ]
; CHECK-NEXT:    [[VEC_IND8:%.*]] = phi <4 x i8> [ [[INDUCTION]], [[VEC_EPILOG_PH]] ], [ [[VEC_IND_NEXT9:%.*]], [[VEC_EPILOG_VECTOR_BODY]] ]
; CHECK-NEXT:    [[OFFSET_IDX10:%.*]] = add i64 3, [[INDEX7]]
; CHECK-NEXT:    [[TMP10:%.*]] = getelementptr inbounds i8, ptr [[A]], i64 [[OFFSET_IDX10]]
; CHECK-NEXT:    store <4 x i8> [[VEC_IND8]], ptr [[TMP10]], align 1
; CHECK-NEXT:    [[INDEX_NEXT11]] = add nuw i64 [[INDEX7]], 4
; CHECK-NEXT:    [[VEC_IND_NEXT9]] = add <4 x i8> [[VEC_IND8]], splat (i8 4)
; CHECK-NEXT:    [[TMP12:%.*]] = icmp eq i64 [[INDEX_NEXT11]], [[N_VEC3]]
; CHECK-NEXT:    br i1 [[TMP12]], label [[VEC_EPILOG_MIDDLE_BLOCK:%.*]], label [[VEC_EPILOG_VECTOR_BODY]], !llvm.loop [[LOOP3:![0-9]+]]
; CHECK:       vec.epilog.middle.block:
; CHECK-NEXT:    [[CMP_N12:%.*]] = icmp eq i64 [[TMP3]], [[N_VEC3]]
; CHECK-NEXT:    br i1 [[CMP_N12]], label [[DOT_CRIT_EDGE_LOOPEXIT]], label [[VEC_EPILOG_SCALAR_PH]]
; CHECK:       vec.epilog.scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL6:%.*]] = phi i64 [ [[IND_END4]], [[VEC_EPILOG_MIDDLE_BLOCK]] ], [ [[IND_END5]], [[VEC_EPILOG_ITER_CHECK]] ], [ 3, [[ITER_CHECK]] ]
; CHECK-NEXT:    br label [[DOTLR_PH:%.*]]
; CHECK:       .lr.ph:
; CHECK-NEXT:    [[INDVARS_IV:%.*]] = phi i64 [ [[INDVARS_IV_NEXT:%.*]], [[DOTLR_PH]] ], [ [[BC_RESUME_VAL6]], [[VEC_EPILOG_SCALAR_PH]] ]
; CHECK-NEXT:    [[TMP13:%.*]] = trunc i64 [[INDVARS_IV]] to i8
; CHECK-NEXT:    [[TMP14:%.*]] = getelementptr inbounds i8, ptr [[A]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    store i8 [[TMP13]], ptr [[TMP14]], align 1
; CHECK-NEXT:    [[INDVARS_IV_NEXT]] = add i64 [[INDVARS_IV]], 1
; CHECK-NEXT:    [[LFTR_WIDEIV:%.*]] = trunc i64 [[INDVARS_IV_NEXT]] to i32
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i32 [[LFTR_WIDEIV]], [[N]]
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[DOT_CRIT_EDGE_LOOPEXIT]], label [[DOTLR_PH]], !llvm.loop [[LOOP4:![0-9]+]]
; CHECK:       ._crit_edge.loopexit:
; CHECK-NEXT:    br label [[DOT_CRIT_EDGE]]
; CHECK:       ._crit_edge:
; CHECK-NEXT:    ret i32 undef
;
  %1 = icmp sgt i32 %n, 3
  br i1 %1, label %.lr.ph, label %._crit_edge

.lr.ph:                                           ; preds = %0, %.lr.ph
  %indvars.iv = phi i64 [ %indvars.iv.next, %.lr.ph ], [ 3, %0 ]
  %2 = trunc i64 %indvars.iv to i8
  %3 = getelementptr inbounds i8, ptr %A, i64 %indvars.iv
  store i8 %2, ptr %3, align 1
  %indvars.iv.next = add i64 %indvars.iv, 1
  %lftr.wideiv = trunc i64 %indvars.iv.next to i32
  %exitcond = icmp eq i32 %lftr.wideiv, %n
  br i1 %exitcond, label %._crit_edge, label %.lr.ph

._crit_edge:                                      ; preds = %.lr.ph, %0
  ret i32 undef
}

define i32 @conversion_cost2(i32 %n, ptr nocapture %A, ptr nocapture %B) nounwind uwtable ssp {
; CHECK-LABEL: @conversion_cost2(
; CHECK-NEXT:    [[TMP1:%.*]] = icmp sgt i32 [[N:%.*]], 9
; CHECK-NEXT:    br i1 [[TMP1]], label [[DOTLR_PH_PREHEADER:%.*]], label [[DOT_CRIT_EDGE:%.*]]
; CHECK:       .lr.ph.preheader:
; CHECK-NEXT:    [[TMP2:%.*]] = add i32 [[N]], -9
; CHECK-NEXT:    [[TMP3:%.*]] = zext i32 [[TMP2]] to i64
; CHECK-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 [[TMP3]], 8
; CHECK-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    [[N_MOD_VF:%.*]] = urem i64 [[TMP3]], 8
; CHECK-NEXT:    [[N_VEC:%.*]] = sub i64 [[TMP3]], [[N_MOD_VF]]
; CHECK-NEXT:    [[IND_END:%.*]] = add i64 9, [[N_VEC]]
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[VEC_IND:%.*]] = phi <2 x i64> [ <i64 9, i64 10>, [[VECTOR_PH]] ], [ [[VEC_IND_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[STEP_ADD:%.*]] = add <2 x i64> [[VEC_IND]], splat (i64 2)
; CHECK-NEXT:    [[STEP_ADD_2:%.*]] = add <2 x i64> [[STEP_ADD]], splat (i64 2)
; CHECK-NEXT:    [[STEP_ADD_3:%.*]] = add <2 x i64> [[STEP_ADD_2]], splat (i64 2)
; CHECK-NEXT:    [[OFFSET_IDX:%.*]] = add i64 9, [[INDEX]]
; CHECK-NEXT:    [[TMP8:%.*]] = add nsw <2 x i64> [[VEC_IND]], splat (i64 3)
; CHECK-NEXT:    [[TMP9:%.*]] = add nsw <2 x i64> [[STEP_ADD]], splat (i64 3)
; CHECK-NEXT:    [[TMP10:%.*]] = add nsw <2 x i64> [[STEP_ADD_2]], splat (i64 3)
; CHECK-NEXT:    [[TMP11:%.*]] = add nsw <2 x i64> [[STEP_ADD_3]], splat (i64 3)
; CHECK-NEXT:    [[TMP12:%.*]] = sitofp <2 x i64> [[TMP8]] to <2 x float>
; CHECK-NEXT:    [[TMP18:%.*]] = sitofp <2 x i64> [[TMP9]] to <2 x float>
; CHECK-NEXT:    [[TMP19:%.*]] = sitofp <2 x i64> [[TMP10]] to <2 x float>
; CHECK-NEXT:    [[TMP20:%.*]] = sitofp <2 x i64> [[TMP11]] to <2 x float>
; CHECK-NEXT:    [[TMP13:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 [[OFFSET_IDX]]
; CHECK-NEXT:    [[TMP15:%.*]] = getelementptr inbounds float, ptr [[TMP13]], i32 2
; CHECK-NEXT:    [[TMP16:%.*]] = getelementptr inbounds float, ptr [[TMP13]], i32 4
; CHECK-NEXT:    [[TMP17:%.*]] = getelementptr inbounds float, ptr [[TMP13]], i32 6
; CHECK-NEXT:    store <2 x float> [[TMP12]], ptr [[TMP13]], align 4
; CHECK-NEXT:    store <2 x float> [[TMP18]], ptr [[TMP15]], align 4
; CHECK-NEXT:    store <2 x float> [[TMP19]], ptr [[TMP16]], align 4
; CHECK-NEXT:    store <2 x float> [[TMP20]], ptr [[TMP17]], align 4
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 8
; CHECK-NEXT:    [[VEC_IND_NEXT]] = add <2 x i64> [[STEP_ADD_3]], splat (i64 2)
; CHECK-NEXT:    [[TMP24:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; CHECK-NEXT:    br i1 [[TMP24]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP5:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    [[CMP_N:%.*]] = icmp eq i64 [[TMP3]], [[N_VEC]]
; CHECK-NEXT:    br i1 [[CMP_N]], label [[DOT_CRIT_EDGE_LOOPEXIT:%.*]], label [[SCALAR_PH]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[IND_END]], [[MIDDLE_BLOCK]] ], [ 9, [[DOTLR_PH_PREHEADER]] ]
; CHECK-NEXT:    br label [[DOTLR_PH:%.*]]
; CHECK:       .lr.ph:
; CHECK-NEXT:    [[INDVARS_IV:%.*]] = phi i64 [ [[INDVARS_IV_NEXT:%.*]], [[DOTLR_PH]] ], [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ]
; CHECK-NEXT:    [[ADD:%.*]] = add nsw i64 [[INDVARS_IV]], 3
; CHECK-NEXT:    [[TOFP:%.*]] = sitofp i64 [[ADD]] to float
; CHECK-NEXT:    [[GEP:%.*]] = getelementptr inbounds float, ptr [[B]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    store float [[TOFP]], ptr [[GEP]], align 4
; CHECK-NEXT:    [[INDVARS_IV_NEXT]] = add i64 [[INDVARS_IV]], 1
; CHECK-NEXT:    [[LFTR_WIDEIV:%.*]] = trunc i64 [[INDVARS_IV_NEXT]] to i32
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i32 [[LFTR_WIDEIV]], [[N]]
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[DOT_CRIT_EDGE_LOOPEXIT]], label [[DOTLR_PH]], !llvm.loop [[LOOP6:![0-9]+]]
; CHECK:       ._crit_edge.loopexit:
; CHECK-NEXT:    br label [[DOT_CRIT_EDGE]]
; CHECK:       ._crit_edge:
; CHECK-NEXT:    ret i32 undef
;
  %1 = icmp sgt i32 %n, 9
  br i1 %1, label %.lr.ph, label %._crit_edge

.lr.ph:                                           ; preds = %0, %.lr.ph
  %indvars.iv = phi i64 [ %indvars.iv.next, %.lr.ph ], [ 9, %0 ]
  %add = add nsw i64 %indvars.iv, 3
  %tofp = sitofp i64 %add to float
  %gep = getelementptr inbounds float, ptr %B, i64 %indvars.iv
  store float %tofp, ptr %gep, align 4
  %indvars.iv.next = add i64 %indvars.iv, 1
  %lftr.wideiv = trunc i64 %indvars.iv.next to i32
  %exitcond = icmp eq i32 %lftr.wideiv, %n
  br i1 %exitcond, label %._crit_edge, label %.lr.ph

._crit_edge:                                      ; preds = %.lr.ph, %0
  ret i32 undef
}
