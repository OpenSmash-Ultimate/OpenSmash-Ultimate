from pathlib import Path
import unittest
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


sys.path.insert(0, str(ROOT / 'tools'))
import cc


class ShaderTimingTests(unittest.TestCase):
    def test_translation_runs_on_workers_without_render_waits(self):
        source = (ROOT / 'runtime/gpu_vk.c').read_text()
        start = source.index('static VShader *find_shader_new(')
        end = source.index('\nstatic ', start + 1)
        block = source[start:end]
        for token in ('shader_translate(', 'translation_load(', 'budget_until('):
            self.assertNotIn(token, block)
        self.assertIn('job_add(&s->job)', block)
        self.assertIn('if (wait_makes()) wait_for(&s->state)', block)
        self.assertIn('memcpy(in->program + skip, p + skip, size - skip)', block)
        self.assertIn('read_gpu(stage_cb[i].address, cb_bytes, in->cb[i])', block)
        self.assertIn('stage_cb[i].size > 0x10000 ? 0x10000', block)
        start = source.index('static void shader_job(')
        worker = source[start:source.index('static int make_log_left(void);', start)]
        self.assertLess(worker.index('translation_load(s)'), worker.index('shader_translate('))
        self.assertLess(worker.index('s->table_hash ='), worker.index('InterlockedExchange(&s->translated, 1)'))
        self.assertIn('translation_input_read, in', worker)
        self.assertIn('if (!s->state) shader_make(s, hlsl)', worker)
        self.assertIn('vs && vs->state == 1 && ps_g', source)
        self.assertNotIn('budget_until(', source)

    def test_attribution_is_in_the_same_bounded_log_as_program_cost(self):
        source = (ROOT / 'runtime/gpu_vk.c').read_text()
        start = source.index('static void make_note_shader(')
        end = source.index('\n}', start)
        block = source[start:end]
        self.assertLess(block.index('!make_log_left()'), block.index('program cost:'))
        self.assertIn('gfx_stat_presented', block)
        self.assertIn('rt_clock_ns() / 1e6', block)

    def test_snapshot_reader_owns_input_and_rejects_out_of_bounds(self):
        source = (ROOT / 'runtime/gpu_vk.c').read_text()
        block = source[source.index('typedef struct TranslationInput {'):source.index('typedef struct VShader {')]
        harness = '#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\ntypedef uint8_t u8;\ntypedef uint32_t u32;\n' + block + '\nint main(void) {\n    TranslationInput *in = calloc(1, sizeof *in);\n    u32 live = 123;\n    in->cb[1] = malloc(4); in->cb_size[1] = 4;\n    memcpy(in->cb[1], &live, 4); live = 456;\n    if (translation_input_read(in, 1, 0) != 123) return 1;\n    if (translation_input_read(in, 1, 1)) return 2;\n    if (translation_input_read(in, 1, UINT32_MAX)) return 3;\n    if (translation_input_read(in, 18, 0)) return 4;\n    if (translation_input_read(in, 0, 0)) return 5;\n    in->cb_size[1] = 3;\n    if (translation_input_read(in, 1, 0)) return 6;\n    translation_input_free(in);\n    translation_input_free(NULL);\n    return 0;\n}\n'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            src, exe = path / 'snapshot.c', path / 'snapshot.exe'
            src.write_text(harness)
            run = subprocess.run(
                [cc.CLANG, '/nologo', *cc.LINK_FLAGS, str(src),
                 '/Fo' + directory + os.sep, '/Fe' + str(exe),
                 '/link', *cc.LINK_OPTIONS] if os.name == 'nt' else
                ['cc', '-std=c11', str(src), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run([str(exe)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
