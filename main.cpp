#include <boost/circular_buffer.hpp>
#include <boost/thread.hpp>
#include <boost/thread/scoped_thread.hpp>
#include <boost/thread/lockable_adapter.hpp>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include <iostream>
#include <fstream>
#include <memory>
#include <vector>

typedef signed short PCM16;

// Normalize Signed 16 bit value
float Pcm16ToFloat(PCM16 value)
{
    return 2.0f * ((float)value - INT16_MIN) / (INT16_MAX - INT16_MIN) - 1.0f;
}

// Calculate relative dB
float Pcm16ToDecibels(PCM16 value)
{
    return 20.0f * log10f(fabsf(Pcm16ToFloat(value)));
}

// Convert two bytes to PCM16
PCM16 BytesToPcm16(uint8_t msbyte, uint8_t lsbyte)
{
    return ((PCM16)msbyte << 8) | lsbyte;
}

// GLFW error callback
void ErrorCallback(int error, const char *description)
{
    std::cerr << "Error: " << description << std::endl;
}

#define VALID_GL_ID(id) id != 0
#define INVALID_GL_ID(id) id == 0

// Returns true if shader is compiled without errors
bool ShaderIsCompiled(GLuint shader)
{
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    return success == GL_TRUE;
}

// Prints the shader info log to given ostream
void PrintShaderLog(std::ostream &out, GLuint shader)
{
    char log[1024];
    glGetShaderInfoLog(shader, 1024, NULL, log);
    out << log << std::endl;
}

// Creates a shader of given type from source GLSL code
GLuint CreateShader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    if (VALID_GL_ID(shader))
    {
        glShaderSource(shader, 1, &src, NULL);
        glCompileShader(shader);
    }
    return shader;
}

// Returns true if program is linked without errors
bool ProgramIsLinked(GLuint program)
{
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    return success == GL_TRUE;
}

// Prints the program info log to given ostream
void PrintProgramLog(std::ostream &out, GLuint program)
{
    char log[1024];
    glGetProgramInfoLog(program, 1024, NULL, log);
    out << log << std::endl;
}

// Creates an OpenGL shader program using a previously compiled vertex and fragment shader
GLuint CreateProgram(GLuint vertShader, GLuint fragShader)
{
    GLuint program = glCreateProgram();
    if (VALID_GL_ID(program))
    {
        glAttachShader(program, vertShader);
        glAttachShader(program, fragShader);
        glLinkProgram(program);
        glDetachShader(program, vertShader);
        glDetachShader(program, fragShader);
    }
    return program;
}

class PaSimpleStream
{
public:
    PaSimpleStream(const std::string &name, const std::string &streamName, const pa_sample_spec &spec) noexcept(false)
    {
        int error;
        stream = pa_simple_new(NULL, name.c_str(), PA_STREAM_RECORD, NULL, streamName.c_str(), &spec, NULL, NULL, &error);
        if (!stream)
        {
            std::string errorString = pa_strerror(error);
            throw std::runtime_error("pa_simple_new error:" + errorString);
        }
    }
    PaSimpleStream(pa_simple *s) : stream(s) {}
    ~PaSimpleStream()
    {
        if (stream)
        {
            pa_simple_free(stream);
        }
    }
    operator pa_simple *() const { return stream; }

    pa_simple *GetStream() { return stream; }

private:
    pa_simple *stream;
};

// Target maximum FPS
const double FPS_LIMIT = 1.0 / 60.0;
// Rate to sample from input device (in this case the default output device from pulse)
const size_t SAMPLE_RATE = 44100;
// Buffer size to use when sampling audio (set according to target fps, to stay in sync with display)
const size_t AUDIO_FRAMEBUF_SIZE = 2 * (size_t(SAMPLE_RATE * FPS_LIMIT) + 2 - size_t(SAMPLE_RATE * FPS_LIMIT) % 2);

// Starting window width
const size_t WIN_WIDTH = 640;
// Starting window height
const size_t WIN_HEIGHT = 480;

// Wrapper for a single uint8_t array that supports copy and move operations
struct AudioSample
{
    AudioSample() {}
    AudioSample(AudioSample &other)
    {
        std::copy(other.data, other.data + capacity, data);
    }
    AudioSample(AudioSample &&other)
    {
        std::copy(other.data, other.data + capacity, data);
        std::memset(other.data, 0, capacity);
    }
    AudioSample &operator=(const AudioSample &other)
    {
        std::copy(other.data, other.data + capacity, data);
    }
    AudioSample &operator=(AudioSample &&other)
    {
        std::copy(other.data, other.data + capacity, data);
        std::memset(other.data, 0, capacity);
    }
    static const size_t capacity = AUDIO_FRAMEBUF_SIZE;
    uint8_t data[capacity];
};

// Container for audio data that can be locked and used across threads
struct AudioBuffer : public boost::basic_lockable_adapter<boost::mutex>
{
    boost::circular_buffer<AudioSample> data;
};

// Allows sampling audio from the pulse audio server (TODO: support files and other devices)
class AudioSampler : public boost::basic_lockable_adapter<boost::mutex>
{
public:
    AudioSampler(const std::string &name, const std::string &stream_name);
    ~AudioSampler();

    bool Read(AudioSample &sample);

    AudioSampler(const AudioSampler &) = delete;
    AudioSampler &operator=(const AudioSampler &) = delete;

private:
    std::unique_ptr<PaSimpleStream> stream;
};

AudioSampler::AudioSampler(const std::string &name, const std::string &streamName)
{
    pa_sample_spec sampleSpec = {PA_SAMPLE_S16LE, SAMPLE_RATE, 1};
    stream.reset(new PaSimpleStream(name, streamName, sampleSpec));
}

AudioSampler::~AudioSampler()
{
}

bool AudioSampler::Read(AudioSample &sample)
{
    int error = 0;
    pa_simple_read(stream->GetStream(), sample.data, AUDIO_FRAMEBUF_SIZE, &error);
    if (error != 0)
    {
        return false;
    }
    return true;
}

// Interface for classes that provide readable audio data
class AudioSource
{
public:
    AudioSource(const std::string &name);

    virtual bool Read(AudioSample &sample) = 0;

    virtual bool IsOpen();

protected:
    const std::string name;
    bool isOpen = false;
};

AudioSource::AudioSource(const std::string &name) : name(name) {}

bool AudioSource::IsOpen()
{
    return isOpen;
}

// Interface for audio sources that are streamed from a persistent source
class StreamingAudioSource : public AudioSource
{
public:
    StreamingAudioSource(const std::string &name);

    virtual void ProcessSound() = 0;

    virtual void Start() = 0;
    virtual void Stop() = 0;
};

StreamingAudioSource::StreamingAudioSource(const std::string &name) : AudioSource(name) {}

// Provides clients the ability to read audio data from the default sound device
class DefaultSoundDevice : public StreamingAudioSource
{
public:
    DefaultSoundDevice(const std::string &name);

    virtual bool Read(AudioSample &sample) override;

    virtual void ProcessSound() override;

    virtual void Start() override;
    virtual void Stop() override;

    DefaultSoundDevice(const DefaultSoundDevice &) = delete;
    DefaultSoundDevice &operator=(const DefaultSoundDevice &) = delete;

private:
    void ProcessSoundLoop();

    size_t readCursor = 0;
    size_t writeCursor = 0;

    AudioBuffer buffer;
    std::unique_ptr<AudioSampler> sampler;
};

DefaultSoundDevice::DefaultSoundDevice(const std::string &name) : StreamingAudioSource(name)
{
    sampler.reset(new AudioSampler(name, "recorder"));
    buffer.data.set_capacity(SAMPLE_RATE / AUDIO_FRAMEBUF_SIZE + 1);
}

bool DefaultSoundDevice::Read(AudioSample &sample)
{
    boost::lock_guard<AudioBuffer> bufferGuard(buffer);

    if (buffer.data.empty())
    {
        return false;
    }

    sample = buffer.data.front();
    buffer.data.pop_front();

    return true;
}

void DefaultSoundDevice::Start()
{
    isOpen = true;
}
void DefaultSoundDevice::Stop()
{
    isOpen = false;
}

void DefaultSoundDevice::ProcessSound()
{
    AudioSample sample;
    std::cout << "ProcessSound thread started\n isOpen? " << isOpen << std::endl;
    while (!isOpen)
    {
        boost::this_thread::sleep(boost::posix_time::millisec(25));
    }
    std::cout << "ProcessSound thread is open" << isOpen << std::endl;
    while (isOpen)
    {
        boost::lock_guard<AudioSampler> samplerGuard(*sampler);
        bool read = sampler->Read(sample);
        boost::lock_guard<AudioBuffer> bufferGuard(buffer);
        buffer.data.push_back(std::move(sample));
    }
}

int main(int argc, char *argv[])
{
    // Initialize audio source
    std::unique_ptr<StreamingAudioSource> audioSource(new DefaultSoundDevice(argv[0]));
    boost::scoped_thread<> audioThread(boost::thread(&StreamingAudioSource::ProcessSound, audioSource.get()));

    // Create rendering window
    GLFWwindow *window = NULL;

    glfwSetErrorCallback(ErrorCallback);

    if (!glfwInit())
    {
        std::cerr << "failed to init GLFW" << std::endl;
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    window = glfwCreateWindow(WIN_WIDTH, WIN_HEIGHT, "hellopulse", NULL, NULL);
    if (!window)
    {
        std::cerr << "failed to init window" << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "failed to load glad" << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwSwapInterval(1);

    // Create shaders and shader program
    const char *vertSrc =
        "#version 330 core\n"
        "in vec2 position;\n"
        "void main(){\n"
        "   gl_Position = vec4(position, 0.0f, 1.0f);\n"
        "}\n";

    const char *fragSrc =
        "#version 330 core\n"
        "out vec4 fragColor;\n"
        "uniform vec4 color;\n"
        "void main(){\n"
        "   fragColor = color;\n"
        "}\n";

    GLuint vertShader = CreateShader(GL_VERTEX_SHADER, vertSrc);
    if (INVALID_GL_ID(vertShader) || !ShaderIsCompiled(vertShader))
    {
        PrintShaderLog(std::cout, vertShader);
        return EXIT_FAILURE;
    }
    GLuint fragShader = CreateShader(GL_FRAGMENT_SHADER, fragSrc);
    if (INVALID_GL_ID(fragShader) || !ShaderIsCompiled(fragShader))
    {
        PrintShaderLog(std::cout, fragShader);
        return EXIT_FAILURE;
    }
    GLuint program = CreateProgram(vertShader, fragShader);
    if (INVALID_GL_ID(program) || !ProgramIsLinked(program))
    {
        PrintProgramLog(std::cout, program);
        return EXIT_FAILURE;
    }
    // save color uniform location for later
    GLint colorUniform = glGetUniformLocation(program, "color");

    // Set lines to be thicker
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glLineWidth(0.5f);

    // Use GL_LEQUAL for depth to allow later draw calls with equal depth to overwrite previous ones
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    // reserve enough space upfront for 1 second of audio with an extra 1 frame buffer
    const size_t numPoints = SAMPLE_RATE + SAMPLE_RATE * FPS_LIMIT;
    std::vector<glm::vec2> channelValues0;
    channelValues0.reserve(numPoints);

    // offset into channelValues buffer (for copying per-frame values)
    size_t offset = 0;
    // number of data points
    size_t count = 0;


    // Create vbo for audio data
    GLuint vbo0;
    glGenBuffers(1, &vbo0);
    if (vbo0 == 0)
    {
        std::cerr << "vbo created with id 0" << std::endl;
        return EXIT_FAILURE;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo0);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec2) * numPoints, nullptr, GL_DYNAMIC_DRAW);

    // Create vao for audio data
    GLuint vao0 = 0;
    glGenVertexArrays(1, &vao0);
    if (vao0 == 0)
    {
        std::cerr << "vao created with id 0" << std::endl;
        return EXIT_FAILURE;
    }
    glBindVertexArray(vao0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo0);
    GLint positionAttrib = glGetAttribLocation(program, "position");
    glVertexAttribPointer(positionAttrib, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(positionAttrib);

    // x position for sample points
    float xPosition = -1.0f;

    double lastTime = glfwGetTime();
    double timer = lastTime;
    double secondsSinceReset = 0;

    int numFrames = 0;
    double deltaTime = 0;

    // use static blue color for lines
    glUseProgram(program);
    glUniform4f(colorUniform, 0.0f, 0.0f, 1.0f, 1.0f);
    glUseProgram(0);

    while (!glfwWindowShouldClose(window))
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update timer
        double currentTime = glfwGetTime();
        deltaTime += (currentTime - lastTime) / FPS_LIMIT;
        lastTime = currentTime;

        if (!audioSource->IsOpen())
        {
            audioSource->Start();
        }

        // Read an audio sample from the device 
        AudioSample sample;
        if (audioSource->Read(sample))
        {
            // Convert audio sample to floating point values
            std::vector<glm::vec2> channelValuesPerFrame0;
            channelValuesPerFrame0.reserve(AUDIO_FRAMEBUF_SIZE / sizeof(PCM16));
            for (int i = 0; i < AUDIO_FRAMEBUF_SIZE; i += sizeof(PCM16)) // read a 16 byte value and store it
            {
                PCM16 s1 = BytesToPcm16(sample.data[i + 1], sample.data[i]);
                //PCM16 s2 = BytesToPcm16(buf[i + 2], buf[i + 3]);
                float x = xPosition;
                float y = Pcm16ToFloat(s1) / 2.0f; // transform range from [-1,1] to [-0.5, 0.5]
                glm::vec2 pos = {x, y};
                channelValues0.push_back(pos);
                channelValuesPerFrame0.push_back(pos);
                count += 1;
                xPosition += float(sizeof(PCM16)) / SAMPLE_RATE;
            }

            // Copy data into vbo
            glBindBuffer(GL_ARRAY_BUFFER, vbo0);
            glBufferSubData(GL_ARRAY_BUFFER, offset, sizeof(glm::vec2) * AUDIO_FRAMEBUF_SIZE / 2, channelValuesPerFrame0.data());
            offset += sizeof(glm::vec2) * AUDIO_FRAMEBUF_SIZE / 2;
        }

        // Draw data points
        glUseProgram(program);
        glBindVertexArray(vao0);
        glDrawArrays(GL_LINE_STRIP, 0, count);
        ++numFrames;

        // display fps once per second
        if (glfwGetTime() - timer >= 1.0)
        {
            ++timer;
            ++secondsSinceReset;
            std::cout << "Fps: " << numFrames << std::endl;
            numFrames = 0;
        }

        if (xPosition > 1.0f)
        {
            // wrap x position around if we've gone past the right side of the screen
            xPosition = -1.0f;

            offset = 0;
            count = 0;
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // ensure clear framebuffer
            // reset the data storage
            glBindBuffer(GL_ARRAY_BUFFER, vbo0);
            glBufferSubData(GL_ARRAY_BUFFER, offset, numPoints, nullptr);
            channelValues0.clear();
            std::cout << "x position wrapped" << std::endl;
            secondsSinceReset = 0;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    audioSource->Stop();

    glfwDestroyWindow(window);

    return EXIT_SUCCESS;
}