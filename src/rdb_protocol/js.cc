#define __STDC_LIMIT_MACROS     // hack. :(
#include <stdint.h>             // for UINT32_MAX

#include "utils.hpp"
#include <boost/make_shared.hpp>

#include "containers/scoped.hpp"
#include "rdb_protocol/jsimpl.hpp"

namespace js {

const id_t MIN_ID = 1;
const id_t MAX_ID = UINT32_MAX;

// ---------- scoped_id_t ----------
scoped_id_t::~scoped_id_t() {
    if (!empty()) reset();
}

void scoped_id_t::reset(id_t id) {
    rassert(id_ != id);
    if (!empty()) {
        parent_->release_id(id_);
    }
    id_ = id;
}


// ---------- env_t ----------
runner_t::req_config_t::req_config_t()
    : timeout_ms(0)
{}

env_t::env_t(extproc::job_t::control_t *control)
    : control_(control),
      should_quit_(false),
      next_id_(MIN_ID)
{}

env_t::~env_t() {
    // Clean up handles.
    for (std::map<id_t, v8::Persistent<v8::Value> >::iterator it = values_.begin();
         it != values_.end();
         ++it)
        it->second.Dispose();
}

void env_t::run() {
    while (!should_quit_) {
        guarantee(-1 != extproc::job_t::accept_job(control_, this));
    }
}

void env_t::shutdown() { should_quit_ = true; }

id_t env_t::rememberValue(v8::Handle<v8::Value> value) {
    id_t id = new_id();
    values_.insert(std::make_pair(id, v8::Persistent<v8::Value>::New(value)));
    return id;
}

v8::Handle<v8::Value> env_t::findValue(id_t id) {
    std::map<id_t, v8::Persistent<v8::Value> >::iterator it = values_.find(id);
    guarantee(it != values_.end());
    return it->second;
}

id_t env_t::new_id() {
    guarantee(next_id_ < MAX_ID); // overflow would be bad
    return next_id_++;
}

void env_t::forget(id_t id) {
    guarantee(id < next_id_);
    guarantee(1 == values_.erase(id));
}


// ---------- runner_t ----------
runner_t::runner_t()
    DEBUG_ONLY(: running_task_(false))
{}

const runner_t::req_config_t *runner_t::default_req_config() {
    static req_config_t config;
    return &config;
}

// TODO(rntz): should we check that we have no used ids? (ie. no remaining
// handles?)
//
// For now, no, because we don't actually use handles to manage id lifetimes at
// the moment. Instead we tag them onto terms and keep them around for the
// entire query.
runner_t::~runner_t() {
    if (connected()) finish();
}

void runner_t::begin(extproc::pool_t *pool) {
    // TODO(rntz): might eventually want to handle external process failure
    guarantee(0 == extproc::job_handle_t::begin(pool, job_t()));
}

void runner_t::interrupt() {
    extproc::job_handle_t::interrupt();
}

struct quit_task_t : auto_task_t<quit_task_t> {
    RDB_MAKE_ME_SERIALIZABLE_0();
    void run(env_t *env) { env->shutdown(); }
};

void runner_t::finish() {
    rassert(connected());
    run_task_t(this, default_req_config(), quit_task_t());
    extproc::job_handle_t::release();
}

// ----- runner_t::job_t -----
void runner_t::job_t::run_job(control_t *control, UNUSED void *extra) {
    // The reason we have env_t is to use it here.
    env_t(control).run();
}

// ----- runner_t::run_task_t -----
runner_t::run_task_t::run_task_t(runner_t *runner, const req_config_t *config, const task_t &task)
    : runner_(runner)
{
    rassert(!runner_->running_task_);
    DEBUG_ONLY_CODE(runner_->running_task_ = true);

    if (NULL == config)
        config = runner->default_req_config();
    if (config->timeout_ms)
        timer_.init(new signal_timer_t(config->timeout_ms));

    guarantee(0 == task.send_over(this));
}

runner_t::run_task_t::~run_task_t() {
    rassert(runner_->running_task_);
    DEBUG_ONLY_CODE(runner_->running_task_ = false);
}

int64_t runner_t::run_task_t::read(void *p, int64_t n) {
    return runner_->read_interruptible(p, n, timer_.has() ? timer_.get() : NULL);
}

int64_t runner_t::run_task_t::write(const void *p, int64_t n) {
    return runner_->write_interruptible(p, n, timer_.has() ? timer_.get() : NULL);
}


// ---------- tasks ----------
// ----- release_id() -----
struct release_task_t : auto_task_t<release_task_t> {
    release_task_t() {}
    explicit release_task_t(id_t id) : id_(id) {}
    id_t id_;
    RDB_MAKE_ME_SERIALIZABLE_1(id_);
    void run(env_t *env) {
        env->forget(id_);
    }
};

void runner_t::release_id(id_t id) {
    rassert(connected());
    rassert(used_ids_.count(id));

    run_task_t(this, default_req_config(), release_task_t(id));

    DEBUG_ONLY_CODE(used_ids_.erase(id));
}

// ----- compile() -----
struct compile_task_t : auto_task_t<compile_task_t> {
    compile_task_t() {}
    compile_task_t(const std::vector<std::string> &args, const std::string &src)
        : args_(args), src_(src) {}

    std::vector<std::string> args_;
    std::string src_;
    RDB_MAKE_ME_SERIALIZABLE_2(args_, src_);

    void mkFuncSrc(scoped_array_t<char> *buf) {
        static const char
            *beg = "(function(",
            *med = "){",
            *end = "})";
        static const ssize_t
            begsz = strlen(beg),
            medsz = strlen(med),
            endsz = strlen(end);

        int nargs = args_.size();
        rassert(args_.size() == (size_t) nargs); // sanity

        ssize_t size = begsz + medsz + endsz + src_.size();
        for (int i = 0; i < nargs; ++i) {
            // + (i > 0) accounts for the preceding comma on extra arguments
            // beyond the first
            size += args_[i].size() + (i > 0);
        }

        buf->init(size);

        char *p = buf->data();
        memcpy(p, beg, begsz);
        p += begsz;

        for (int i = 0; i < nargs; ++i) {
            if (i) *p++ = ',';
            const std::string &s = args_[i];
            memcpy(p, s.data(), s.size());
            p += s.size();
        }

        memcpy(p, med, medsz);
        p += medsz;

        memcpy(p, src_.data(), src_.size());
        p += src_.size();

        memcpy(p, end, endsz);
        rassert(p - buf->data() == size - endsz,
                "\np - buf->data() = %ld\nsize = %ld\nendsz = %lu",
                p - buf->data(),
                size,
                endsz);
    }

    v8::Handle<v8::Function> mkFunc(std::string *errmsg) {
        v8::Handle<v8::Function> result; // initially empty

        // Compile & run script to get a function.
        scoped_array_t<char> srcbuf;
        mkFuncSrc(&srcbuf);
        // TODO(rntz): use an "external resource" to avoid copy?
        v8::Handle<v8::String> src = v8::String::New(srcbuf.data(), srcbuf.size());

        v8::TryCatch try_catch;

        v8::Handle<v8::Script> script = v8::Script::Compile(src);
        if (script.IsEmpty()) {
            // TODO (rntz): use try_catch for error message
            *errmsg = "compiling function definition failed";
            return result;
        }

        v8::Handle<v8::Value> funcv = script->Run();
        if (funcv.IsEmpty()) {
            // TODO (rntz): use try_catch for error message
            *errmsg = "evaluating function definition failed";
            return result;
        }
        if (!funcv->IsFunction()) {
            *errmsg = "evaluating function definition did not produce function";
            return result;
        }
        result = v8::Handle<v8::Function>::Cast(funcv);
        rassert(!result.IsEmpty());
        return result;
    }

    void run(env_t *env) {
        id_result_t result("");
        std::string *errmsg = boost::get<std::string>(&result);

        v8::HandleScope handle_scope;
        // Evaluate the function definition.
        v8::Handle<v8::Function> func = mkFunc(errmsg);
        if (!func.IsEmpty()) {
            result = env->rememberValue(func);
        }

        write_message_t msg;
        msg << result;
        guarantee(0 == send_write_message(env->control(), &msg));
    }
};

id_t runner_t::compile(
    const std::vector<std::string> &args,
    const std::string &source,
    std::string *errmsg,
    const req_config_t *config)
{
    id_result_t result;

    {
        run_task_t run(this, config, compile_task_t(args, source));
        guarantee(ARCHIVE_SUCCESS == deserialize(&run, &result));
    }

    id_visitor_t v(errmsg);
    return new_id(boost::apply_visitor(v, result));
}

// ----- call() -----
struct call_task_t : auto_task_t<call_task_t> {
    call_task_t() {}
    call_task_t(id_t id, const std::vector<boost::shared_ptr<scoped_cJSON_t> > &args)
        : func_id_(id), args_(args) {}

    id_t func_id_;
    std::vector<boost::shared_ptr<scoped_cJSON_t> > args_;
    RDB_MAKE_ME_SERIALIZABLE_2(func_id_, args_);

    v8::Handle<v8::Value> eval(v8::Handle<v8::Function> func, std::string *errmsg) {
        v8::TryCatch try_catch;
        v8::HandleScope scope;

        // Construct receiver object.
        v8::Handle<v8::Object> obj = v8::Object::New();

        // Construct arguments.
        int nargs = (int) args_.size();
        guarantee(args_.size() == (size_t) nargs); // sanity

        scoped_array_t<v8::Handle<v8::Value> > handles(nargs);
        for (int i = 0; i < nargs; ++i) {
            handles[i] = fromJSON(*args_[i]->get());
            rassert(!handles[i].IsEmpty());
        }

        // Call function with environment as its receiver.
        v8::Handle<v8::Value> result = func->Call(obj, nargs, handles.data());
        if (result.IsEmpty()) {
            *errmsg = "calling function failed";
            if (try_catch.HasCaught()) {
                v8::Handle<v8::String> msg = try_catch.Message()->Get();

                // FIXME TODO (rntz): stack overflow problem if len too large.
                size_t len = msg->Utf8Length();
                char buf[len];
                msg->WriteUtf8(buf);

                errmsg->append(":\n");
                errmsg->append(buf, len);
            }
        }
        return scope.Close(result);
    }

    void run(env_t *env) {
        // TODO(rntz): This is very similar to compile_task_t::run(). Refactor?
        json_result_t result("");
        std::string *errmsg = boost::get<std::string>(&result);

        v8::HandleScope handle_scope;
        v8::Handle<v8::Function> func = v8::Handle<v8::Function>::Cast(env->findValue(func_id_));
        rassert(!func.IsEmpty());

        v8::Handle<v8::Value> value = eval(func, errmsg);
        if (!value.IsEmpty()) {
            // JSONify result.
            boost::shared_ptr<scoped_cJSON_t> json = toJSON(value, errmsg);
            if (json) {
                result = json;
            }
        }

        write_message_t msg;
        msg << result;
        guarantee(0 == send_write_message(env->control(), &msg));
    }
};

boost::shared_ptr<scoped_cJSON_t> runner_t::call(
    id_t func_id,
    const std::vector<boost::shared_ptr<scoped_cJSON_t> > &args,
    std::string *errmsg,
    const req_config_t *config)
{
    json_result_t result;

    {
        run_task_t run(this, config, call_task_t(func_id, args));
        guarantee(ARCHIVE_SUCCESS == deserialize(&run, &result));
    }

    json_visitor_t v(errmsg);
    return boost::apply_visitor(v, result);
}

} // namespace js
