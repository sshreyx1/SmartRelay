/*
 * test_sound_model.c - the C classifier (firmware/sensing/sound_classify.c + the generated
 * sound_model.h) must give exactly the label and confidence ml/qmodel.py gives on the same 200
 * test captures (host/tests/sound_model_vectors.h, from ml/export_c.py --vectors). 100% or fail.
 */
#include <stdio.h>

#include "sound_classes.h"
#include "sound_classify.h"
/* DEVIATION (BARR-C 4.2.c.ii, storage allocated in a header file): sound_model_vectors.h is
 * GENERATED test data (ml/export_c.py --vectors) and this host test is its only includer.
 * Approved by: ____________________ (project manager), date: __________ */
#include "sound_model_vectors.h"

int main(void)
{
    int match = 0;
    for (int i = 0; i < SM_N_VECTORS; i++)
    {
        uint8_t label = 0, conf = 0;
        sound_classify(sm_vec_features[i], &label, &conf);
        if (label == sm_vec_label[i] && conf == sm_vec_conf[i])
        {
            match++;
        }
        else
        {
            printf("FAIL vector %d: C %s %u%%, python %s %u%%\n", i, sound_class_name(label), conf,
                   sound_class_name(sm_vec_label[i]), sm_vec_conf[i]);
        }
    }
    printf("sound_model: %d/%d vectors match (min conf %u)\n", match, SM_N_VECTORS, sound_classify_min_conf());
    return match == SM_N_VECTORS ? 0 : 1;
}
