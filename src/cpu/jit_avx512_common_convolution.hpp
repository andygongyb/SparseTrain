/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
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

#ifndef CPU_JIT_AVX512_COMMON_CONVOLUTION_HPP
#define CPU_JIT_AVX512_COMMON_CONVOLUTION_HPP

#include "c_types_map.hpp"
#include "cpu_convolution_pd.hpp"
#include "cpu_engine.hpp"
#include "jit_avx512_common_conv_kernel.hpp"
#include "jit_transpose_src_utils.hpp"
#include "cpu_reducer.hpp"
#include "cpu_barrier.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <bool with_relu, impl::data_type_t src_type,
         impl::data_type_t wei_type = src_type,
         impl::data_type_t dst_type = src_type>
struct _jit_avx512_common_convolution_fwd_t : public cpu_primitive_t {
    struct pd_t : public _cpu_convolution_fwd_pd_t<with_relu> {
        pd_t(engine_t *engine, const typename pd_t::base_desc_t *adesc,
                const primitive_attr_t *attr,
                const typename pd_t::base_class *hint_fwd_pd)
            : _cpu_convolution_fwd_pd_t<with_relu>(engine, adesc, attr,
                    hint_fwd_pd)
            , jcp_()
        {
        }

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit:", avx512_common, ""),
                _jit_avx512_common_convolution_fwd_t);

        virtual status_t init() override
        {
            using namespace prop_kind;
            assert(this->engine()->kind() == engine_kind::cpu);
            bool ok = true
                    && utils::one_of(this->cdesc_().prop_kind, forward_training,
                               forward_inference)
                    && this->cdesc_().alg_kind == alg_kind::convolution_direct
                    && !this->has_zero_dim_memory()
                    && this->cdesc_().src_desc.data_type == src_type
                    && this->cdesc_().weights_desc.data_type == wei_type
                    && this->cdesc_().dst_desc.data_type == dst_type
                    && utils::implication(this->with_bias(), dst_type
                                       == this->cdesc_().bias_desc.data_type)
                    && !(with_relu && this->negative_slope()!= 0.
                                   && dst_type == data_type::s32
                                   && src_type == data_type::s16
                                   && wei_type == data_type::s16);
            if (!ok)
                return status::unimplemented;

            return jit_avx512_common_conv_fwd_kernel::init_conf(
                    jcp_, this->cdesc_(), this->src_pd_, this->weights_pd_,
                    this->dst_pd_,this->bias_pd_, *this->attr(),
                    mkldnn_get_max_threads(), with_relu, this->negative_slope());
        }

        inline int ndims() { return this->cdesc_().src_desc.ndims; }

        jit_conv_conf_t jcp_;
    };

    _jit_avx512_common_convolution_fwd_t(const pd_t *pd,
            const input_vector &inputs, const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
        , padded_bias_(nullptr)
    {
        kernel_ = new jit_avx512_common_conv_fwd_kernel(conf_.jcp_,
                    *conf_.attr());

        const auto &j = conf_.jcp_;

        int nthr = mkldnn_get_max_threads() / 2;

        params_ = new param_t[nthr];

        int step = j.dilate_w == 0 ? j.oc_buffs : 1;
        int oc_iters = j.nb_oc / step;

        int work_amount = j.ngroups * j.oh * j.nb_mb * j.mb_block * oc_iters;

        parallel(nthr, [&](const int ithr, const int nthr) {
            int start, end;
            balance211(work_amount, nthr, ithr, start, end);

            param_t &p = params_[ithr];
            int g{0}, mbb{0}, oh{0}, ocb{0}, mb_s{0};
            
            nd_iterator_init(start, g, j.ngroups, mbb, j.nb_mb, oh, j.oh,
                    ocb, oc_iters, mb_s, j.mb_block);

            p.g[1] = p.g[0] = g;
            p.mbb[1] = p.mbb[0] = mbb;
            p.oh[1] = p.oh[0] = oh;
            p.ocb[1] = p.ocb[0] = ocb;
            p.mb[1] = p.mb[0] = mb_s;

            while (start < end) {

                p.g[1] = g;
                p.mbb[1] = mbb;
                p.oh[1] = oh;
                p.ocb[1] = ocb;

                int work_rem = end - start;

                p.mb[1] = mb_s + work_rem > j.mb_block ? j.mb_block : mb_s + work_rem;

                nd_iterator_jump(start, end, g, j.ngroups, mbb, j.nb_mb, oh, j.oh,
                        ocb, oc_iters, mb_s, j.mb_block);
            }


        });

        if (conf_.want_padded_bias()) {
            assert(j.ngroups == 1);
            padded_bias_ = (dst_data_t *)malloc(sizeof(dst_data_t) * j.oc, 64);
            for (int oc = j.oc_without_padding; oc < j.oc; ++oc)
                padded_bias_[oc] = 0;
        }
    }
    ~_jit_avx512_common_convolution_fwd_t() {
        delete kernel_;
        delete params_;
        free(padded_bias_);
    };

    typedef typename prec_traits<src_type>::type src_data_t;
    typedef typename prec_traits<wei_type>::type wei_data_t;
    typedef typename prec_traits<dst_type>::type dst_data_t;

    virtual void execute(event_t *e)
    {
        if (conf_.ndims() == 4) execute_forward();
        else                    execute_forward_3d();
        e->set_state(event_t::ready);
    }

private:

    typedef struct {

        int g[2];
        int mbb[2];
        int oh[2];
        int ocb[2];
        int mb[2];

    } param_t;

    void execute_forward();
    void execute_forward_3d();
    pd_t conf_;
    jit_avx512_common_conv_fwd_kernel *kernel_;
    param_t *params_;
    dst_data_t *padded_bias_;
};

template <impl::data_type_t src_type, impl::data_type_t wei_type = src_type,
         impl::data_type_t dst_type = src_type>
using jit_avx512_common_convolution_fwd_t =
    _jit_avx512_common_convolution_fwd_t<false, src_type, wei_type, dst_type>;

template <impl::data_type_t src_type, impl::data_type_t wei_type = src_type,
         impl::data_type_t dst_type = src_type>
using jit_avx512_common_convolution_relu_t =
    _jit_avx512_common_convolution_fwd_t<true, src_type, wei_type, dst_type>;

template <impl::data_type_t diff_dst_type,
          impl::data_type_t wei_type = diff_dst_type,
          impl::data_type_t diff_src_type = diff_dst_type>
struct jit_avx512_common_convolution_bwd_data_t: public cpu_primitive_t {
    struct pd_t: public cpu_convolution_bwd_data_pd_t {
        pd_t(engine_t *engine,
                const convolution_desc_t *adesc,
                const primitive_attr_t *attr,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_data_pd_t(engine, adesc, attr, hint_fwd_pd)
            , jcp_()
        {}

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit:", avx512_common, ""),
                jit_avx512_common_convolution_bwd_data_t);

        virtual status_t init() override {
            using namespace prop_kind;
            assert(this->engine()->kind() == engine_kind::cpu);
            bool ok = true
                && this->set_default_params() == status::success
                && utils::one_of(this->desc()->prop_kind, backward_data) // XXX (this->!)
                && !this->has_zero_dim_memory()
                && this->desc()->alg_kind == alg_kind::convolution_direct
                && this->desc()->diff_dst_desc.data_type == diff_dst_type
                && this->desc()->weights_desc.data_type == wei_type
                && this->desc()->diff_src_desc.data_type == diff_src_type;
            if (!ok) return status::unimplemented;

            return jit_avx512_common_conv_bwd_data_kernel_f32::init_conf(
                    jcp_,*this->desc(), *this->diff_src_pd_.desc(),
                    *this->weights_pd_.desc(), *this->diff_dst_pd_.desc());
        }

        inline int ndims() { return this->desc()->diff_src_desc.ndims; }

        inline memory_format_t src_format()
        {
            using namespace memory_format;
            return (ndims() == 4) ? nChw16c : nCdhw16c;
        }
        inline memory_format_t wei_format()
        {
            using namespace memory_format;
            if (diff_dst_type == data_type::s16
                && diff_src_type == data_type::s32
                && wei_type == data_type::s16) {
                return  this->with_groups() ? gOIhw8o16i2o : OIhw8o16i2o;
            } else {
                return (ndims() == 4)
                    ? this->with_groups() ? gOIhw16o16i : OIhw16o16i
                    : this->with_groups() ? gOIdhw16o16i : OIdhw16o16i;
            }
        }

        jit_conv_conf_t jcp_;

    protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;

            if (this->diff_src_pd_.desc()->format == any)
                CHECK(this->diff_src_pd_.set_format(src_format()));
            if (this->diff_dst_pd_.desc()->format == any)
                CHECK(this->diff_dst_pd_.set_format(src_format()));
            if (this->weights_pd_.desc()->format == any)
                CHECK(this->weights_pd_.set_format(wei_format()));
            return status::success;
        }
    };

    jit_avx512_common_convolution_bwd_data_t(const pd_t *pd,
            const input_vector &inputs, const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
    {
        kernel_ = new jit_avx512_common_conv_bwd_data_kernel_f32(conf_.jcp_, false);

        const auto &j = conf_.jcp_;

        int nthr = mkldnn_get_max_threads() / 2;

        params_ = new param_t[nthr];

        int ic_iters = j.nb_ic / j.ic_buffs;

        int work_amount = j.ngroups * j.ih * j.nb_mb * j.mb_block * ic_iters;

        parallel(nthr, [&](const int ithr, const int nthr) {
            int start, end;
            balance211(work_amount, nthr, ithr, start, end);

            param_t &p = params_[ithr];
            int g{0}, mbb{0}, ih{0}, icb{0}, mb_s{0};
            
            nd_iterator_init(start, g, j.ngroups, mbb, j.nb_mb, ih, j.ih,
                    icb, ic_iters, mb_s, j.mb_block);

            p.g[1] = p.g[0] = g;
            p.mbb[1] = p.mbb[0] = mbb;
            p.ih[1] = p.ih[0] = ih;
            p.icb[1] = p.icb[0] = icb;
            p.mb[1] = p.mb[0] = mb_s;

            while (start < end) {

                p.g[1] = g;
                p.mbb[1] = mbb;
                p.ih[1] = ih;
                p.icb[1] = icb;

                int work_rem = end - start;

                p.mb[1] = mb_s + work_rem > j.mb_block ? j.mb_block : mb_s + work_rem;

                nd_iterator_jump(start, end, g, j.ngroups, mbb, j.nb_mb, ih, j.ih,
                        icb, ic_iters, mb_s, j.mb_block);
            }


        });

    }
    ~jit_avx512_common_convolution_bwd_data_t() {
        delete kernel_;
        delete params_;
    };

    typedef typename prec_traits<diff_dst_type>::type diff_dst_data_t;
    typedef typename prec_traits<wei_type>::type wei_data_t;
    typedef typename prec_traits<diff_src_type>::type diff_src_data_t;

    virtual void execute(event_t *e) {
        switch (conf_.desc()->prop_kind) {
        case prop_kind::backward_data:
            if (conf_.ndims() == 4) execute_backward_data();
            else                    execute_backward_data_3d();
            break;
        default:
            assert(!"invalid prop_kind");
        }
        e->set_state(event_t::ready);
    }

private:

    typedef struct {

        int g[2];
        int mbb[2];
        int ih[2];
        int icb[2];
        int mb[2];

    } param_t;

    void execute_backward_data();
    void execute_backward_data_3d();
    pd_t conf_;
    jit_avx512_common_conv_bwd_data_kernel_f32 *kernel_;
    param_t *params_;
};

template <impl::data_type_t src_type,
          impl::data_type_t diff_dst_type = src_type,
          impl::data_type_t diff_weights_type = src_type>
struct jit_avx512_common_convolution_bwd_weights_t: public cpu_primitive_t {
    struct pd_t: public  cpu_convolution_bwd_weights_pd_t {
        pd_t(engine_t *engine, const convolution_desc_t *adesc,
                const primitive_attr_t *attr,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_weights_pd_t(engine, adesc, attr, hint_fwd_pd)
            , jcp_() {}

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit:", avx512_common, ""),
                jit_avx512_common_convolution_bwd_weights_t);

        virtual status_t init() override {
            assert(this->engine()->kind() == engine_kind::cpu);
            bool ok = true
                && this->desc()->prop_kind == prop_kind::backward_weights
                && this->desc()->alg_kind == alg_kind::convolution_direct
                && !this->has_zero_dim_memory()
                && this->desc()->src_desc.data_type == src_type
                && this->desc()->diff_dst_desc.data_type == diff_dst_type
                && this->desc()->diff_weights_desc.data_type
                    == diff_weights_type;
            if (!ok) return status::unimplemented;

            return jit_avx512_common_conv_bwd_weights_kernel_f32::init_conf(
                    jcp_, *this->desc(), this->src_pd_, this->diff_weights_pd_,
                    this->diff_bias_pd_, this->diff_dst_pd_);
        }

        inline int ndims() { return this->desc()->src_desc.ndims; }

        inline memory_format_t src_format()
        {
            using namespace memory_format;
            return (ndims() == 4) ? nChw16c : nCdhw16c;
        }
        inline memory_format_t wei_format()
        {
            using namespace memory_format;
            return (ndims() == 4)
                ? this->with_groups() ? gOIhw16o16i : OIhw16o16i
                : this->with_groups() ? gOIdhw16o16i : OIdhw16o16i;
        }


        jit_conv_conf_t jcp_;

        protected:
            virtual status_t set_default_params() override {
                using namespace memory_format;

                if (this->src_pd_.desc()->format == any)
                    CHECK(this->src_pd_.set_format(src_format()));
                if (this->diff_weights_pd_.desc()->format == any)
                    CHECK(this->diff_weights_pd_.set_format(wei_format()));
                if (this->diff_dst_pd_.desc()->format == any)
                    CHECK(this->diff_dst_pd_.set_format(src_format()));

                return status::success;
            }

    };

    jit_avx512_common_convolution_bwd_weights_t(const pd_t *pd,
            const input_vector &inputs, const output_vector &outputs);
    ~jit_avx512_common_convolution_bwd_weights_t() {

        delete kernel_;
        delete params_;
        if (trans_kernel_)
            delete trans_kernel_;
        if (acc_ker_)
            delete acc_ker_;
        delete reducer_bias_;

        //free(src_workspace_);
        free(dst_workspace_);

        free(padded_bias_);

        free(tr_src_);
        free(ws_reduction_);

        free(tr_src_bctx_);
        free(tr_diff_dst_bctx_);
    }

    typedef typename prec_traits<src_type>::type src_data_t;
    typedef typename prec_traits<diff_dst_type>::type diff_dst_data_t;
    typedef typename prec_traits<diff_weights_type>::type diff_weights_data_t;

    virtual void execute(event_t *e) {
        execute_backward_weights();
        e->set_state(event_t::ready);
    }

private:
    void execute_backward_weights();
    void balance();

    struct thread_info_t;
    void compute_diff_weights(const thread_info_t *);
    void compute_diff_weights_3d(const thread_info_t *);
    void reduce_diff_weights(const thread_info_t *);
    void reduce_diff_weights_3d(const thread_info_t *);
    void compute_diff_bias(const thread_info_t *);
    void compute_diff_bias_3d(const thread_info_t *);

    typedef struct {

        int g[2];
        int ocb[2];
        int ic[2];

    } param_t;

    param_t *params_;

    pd_t conf_;

    jit_avx512_common_conv_bwd_weights_kernel_f32 *kernel_;
    jit_trans_src_t *trans_kernel_;
    jit_trans_dst_t *trans_dst_kernel_;
    cpu_accumulator_1d_t<diff_weights_type> *acc_ker_;
    cpu_reducer_t<diff_weights_type> *reducer_bias_;
    diff_weights_data_t *padded_bias_;

    src_data_t *tr_src_;
    diff_dst_data_t *tr_diff_dst_;
    diff_weights_data_t *ws_reduction_;

    src_data_t *src_workspace_;
    diff_dst_data_t *dst_workspace_;

    int nthr_, nthr_mb_, nthr_g_, nthr_oc_b_, nthr_ic_b_;
    simple_barrier::ctx_t *tr_src_bctx_, *tr_diff_dst_bctx_, reduction_bctx_;
};

}
}
}

#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
