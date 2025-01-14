/*******************************************************************************
* Copyright 2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oneapi/dnnl/dnnl.h"

#include "dnnl_common.hpp"
#include "dnnl_debug.hpp"

#include "brgemm/brgemm.hpp"

namespace brgemm {

dnnl_data_type_t prb_t::get_dt(data_kind_t data_kind) const {
    switch (data_kind) {
        case SRC: return src_dt();
        case WEI: return wei_dt();
        case BIA: return bia_dt;
        case DST: return dst_dt();
        case ACC: return acc_dt();
        default: assert(!"unexpected"); return dnnl_data_type_undef;
    }
}

void prb_t::generate_oscales() {
    // Brgemm takes single pointer oscale, but relies on a combination of arg
    // scales attributes. This helps to reuse attributes from primitives, but
    // requires them to pre-compute oscale = src_scale * wei_scale[:]
    const auto &attr_scales = attr.scales;

    const auto &src_sc = attr_scales.get(DNNL_ARG_SRC);
    float src_scale_val = 1.0f;
    if (!src_sc.is_def()) {
        assert(src_sc.policy == policy_t::COMMON);
        src_scale_val = src_sc.scale;
    }

    const auto &wei_sc = attr_scales.get(DNNL_ARG_WEIGHTS);

    if (wei_sc.policy == policy_t::COMMON) {
        scales = (float *)zmalloc(sizeof(float), 4);
        SAFE_V(scales != nullptr ? OK : FAIL);
        scales[0] = wei_sc.scale;
        if (!src_sc.is_def()) { scales[0] *= src_scale_val; }
        return;
    }

    assert(wei_sc.policy == policy_t::PER_OC);

    scales = (float *)zmalloc(sizeof(float) * n, 64);
    SAFE_V(scales != nullptr ? OK : FAIL);

    const float K = 32;
    /* scale in [1/K .. K], with starting point at wei_sc.scale */
    float s[2] = {wei_sc.scale, wei_sc.scale / 2};
    for (int64_t i = 0; i < n; ++i) {
        int64_t si = i % 2; // 0 -> left, 1 -> right
        scales[i] = s[si] * src_scale_val;
        if (si == 0) {
            s[si] /= 2.;
            if (s[si] < 1. / K) s[si] *= K * K; // turn around to become ~K
        } else {
            s[si] *= 2.;
            if (s[si] > K) s[si] /= K * K; // turn around to become ~K
        }
    }
}

int32_t *prb_t::generate_zero_points(
        int arg, const attr_t::zero_points_t &zero_points, int N) {
    if (zero_points.is_def(arg)) return nullptr;

    const auto &e = zero_points.get(arg);
    if (e.policy == policy_t::COMMON) {
        int32_t *zp = (int32_t *)zmalloc(sizeof(int32_t), 4);
        SAFE_V(zp != nullptr ? OK : FAIL);
        zp[0] = e.value;
        return zp;
    }

    assert(e.policy == policy_t::PER_DIM_1);

    int32_t *zp = (int32_t *)zmalloc(sizeof(int32_t) * N, 64);
    SAFE_V(zp != nullptr ? OK : FAIL);

    for (int i = 0; i < N; ++i)
        zp[i] = e.value + i % 3;
    return zp;
}

std::ostream &operator<<(std::ostream &s, const prb_t &prb) {
    dump_global_params(s);
    settings_t def;

    bool has_default_dts = true;
    for (const auto &i_dt : prb.dt)
        has_default_dts = has_default_dts && i_dt == dnnl_f32;

    if (canonical || !has_default_dts) s << "--dt=" << prb.dt << " ";
    if (canonical || prb.stag != def.stag[0]) s << "--stag=" << prb.stag << " ";
    if (canonical || prb.wtag != def.wtag[0]) s << "--wtag=" << prb.wtag << " ";
    if (canonical || prb.dtag != def.dtag[0]) s << "--dtag=" << prb.dtag << " ";
    if (canonical || prb.ld != def.ld[0]) {
        s << "--ld=";
        if (prb.ld[0] != 0) s << prb.ld[0];
        s << ":";
        if (prb.ld[1] != 0) s << prb.ld[1];
        s << ":";
        if (prb.ld[2] != 0) s << prb.ld[2];
        s << " ";
    }

    if (canonical || prb.bia_dt != def.bia_dt[0])
        s << "--bia_dt=" << prb.bia_dt << " ";

    if (canonical || prb.alpha != def.alpha[0])
        s << "--alpha=" << prb.alpha << " ";
    if (canonical || prb.beta != def.beta[0]) s << "--beta=" << prb.beta << " ";
    if (canonical || prb.batch_size != def.batch_size[0])
        s << "--bs=" << prb.batch_size << " ";
    if (canonical || prb.brgemm_attr != def.brgemm_attr[0])
        s << "--brgemm-attr=" << prb.brgemm_attr << " ";

    s << prb.attr;
    s << static_cast<const prb_vdims_t &>(prb);

    return s;
}

} // namespace brgemm
