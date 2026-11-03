"""
qmodel.py - the deployed (quantised) form of the two-stage tree classifier, in Python.

It computes exactly what firmware/sensing/sound_classify.c computes, so offline metrics are the
node's metrics and export_c.py --vectors can demand a 100% match:
  - traversal: sklearn's own apply() on float32 features. sklearn compares (double)x <= threshold;
    the C code compares x <= thr32 with thr32 = the largest float32 <= threshold, which is the
    same test for every float32 x (export_c.py rounds thresholds down).
  - each leaf stores its class distribution as uint8 percent (round(100 * p));
  - a forest sums its trees' percents per class (ties go to the lowest class index) and
    conf = (best_sum + n_trees // 2) // n_trees  (0-100).
  - end to end: stage 1 gives a group (conf c1); for an animal group stage 2 gives a label (conf c2).
    The label's confidence is the combined c = (c1 * c2 + 50) // 100, about P(group) * P(label | group),
    so a capture stage 1 was unsure about cannot get a confident animal label. The label is kept if
    c >= min_conf, else the output is the group id with c1.
"""
from dataclasses import dataclass, field

import numpy as np


@dataclass
class QTree:
    feature: np.ndarray    # per sklearn node, -2 for leaves
    threshold: np.ndarray  # float64, sklearn's
    left: np.ndarray
    right: np.ndarray
    leaf_pct: dict         # sklearn node id -> np.ndarray[uint8] (n_classes)


@dataclass
class QModel:
    """One stage: a DecisionTree (1 tree) or a forest, quantised."""
    est: object            # the fitted sklearn estimator (for apply())
    classes: list          # label ids, in sklearn class order
    trees: list = field(default_factory=list)

    @classmethod
    def from_sklearn(cls, est):
        q = cls(est=est, classes=[int(c) for c in est.classes_])
        for t in getattr(est, "estimators_", [est]):
            tr = t.tree_
            pct = {}
            for node in np.where(tr.children_left == -1)[0]:
                v = tr.value[node][0].astype(np.float64)
                p = v / v.sum() if v.sum() > 0 else np.zeros_like(v)
                pct[int(node)] = np.round(p * 100).astype(np.uint8)
            q.trees.append(QTree(tr.feature.copy(), tr.threshold.copy(), tr.children_left.copy(),
                                 tr.children_right.copy(), pct))
        return q

    @property
    def n_nodes(self) -> int:
        return int(sum(len(t.feature) for t in self.trees))

    def flash_bytes(self) -> int:
        """Approximate C size: internal node = int8 feat + float thr + 2 x int16 = 9 B; leaf = n_classes B."""
        total = 0
        for t in self.trees:
            leaves = len(t.leaf_pct)
            total += (len(t.feature) - leaves) * 9 + leaves * len(self.classes)
        return total

    def _leaves(self, X: np.ndarray) -> np.ndarray:
        X = np.asarray(X, dtype=np.float32)
        ests = getattr(self.est, "estimators_", None)
        if ests is None:
            return self.est.apply(X)[:, None]
        return np.stack([e.apply(X) for e in ests], axis=1)

    def predict(self, X: np.ndarray):
        """-> (label ids np.ndarray[int], conf np.ndarray[int] 0-100), exactly like sm_predict() in C."""
        leaves = self._leaves(X)
        n_trees = len(self.trees)
        votes = np.zeros((len(leaves), len(self.classes)), dtype=np.int64)
        for j, t in enumerate(self.trees):
            votes += np.stack([t.leaf_pct[int(n)] for n in leaves[:, j]]).astype(np.int64)
        best = votes.argmax(axis=1)  # first max = lowest class index, like the C loop
        conf = (votes[np.arange(len(votes)), best] + n_trees // 2) // n_trees
        return np.array(self.classes)[best], conf


@dataclass
class TwoStage:
    stage1: QModel
    stage2: dict           # group id -> QModel
    min_conf: int          # SOUND_STAGE2_MIN_CONF

    def predict(self, X: np.ndarray):
        """-> (label id, conf, group id from stage 1, stage-2 label or -1, combined stage-2 conf or -1)."""
        X = np.asarray(X, dtype=np.float32)
        g, c1 = self.stage1.predict(X)
        label, conf = g.copy(), c1.copy()
        l2 = np.full(len(X), -1)
        c2 = np.full(len(X), -1)
        for gid, m in self.stage2.items():
            idx = np.where(g == gid)[0]
            if len(idx) == 0:
                continue
            lab, cf = m.predict(X[idx])
            cf = (c1[idx] * cf + 50) // 100
            l2[idx], c2[idx] = lab, cf
            keep = cf >= self.min_conf
            label[idx[keep]] = lab[keep]
            conf[idx[keep]] = cf[keep]
        return label, conf, g, l2, c2
