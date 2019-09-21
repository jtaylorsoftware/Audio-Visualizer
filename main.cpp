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

void ErrorCallback(int error, const char *description)
{
    std::cerr << "Error: " << description << std::endl;
}

#define VALID_GL_ID(id) id != 0
#define INVALID_GL_ID(id) id == 0

GLint ShaderIsCompiled(GLuint shader)
{
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    return success;
}

void PrintShaderLog(std::ostream &out, GLuint shader)
{
    char log[1024];
    glGetShaderInfoLog(shader, 1024, NULL, log);
    out << log << std::endl;
}

GLuint CreateShader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    if (shader != 0)
    {
        glShaderSource(shader, 1, &src, NULL);
        glCompileShader(shader);
    }
    return shader;
}

GLint ProgramIsLinked(GLuint program)
{
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    return success;
}

void PrintProgramLog(std::ostream &out, GLuint program)
{
    char log[1024];
    glGetProgramInfoLog(program, 1024, NULL, log);
    out << log << std::endl;
}

GLuint CreateProgram(GLuint vertShader, GLuint fragShader)
{
    GLuint program = glCreateProgram();
    if (program != 0)
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

const double FPS_LIMIT = 1.0 / 60.0;
const size_t SAMPLE_RATE = 44100;
const size_t BUFSIZE = size_t(SAMPLE_RATE * FPS_LIMIT) + 2 - size_t(SAMPLE_RATE * FPS_LIMIT) % 2;
const size_t WIN_WIDTH = 640;
const size_t WIN_HEIGHT = 480;

int main(int argc, char *argv[])
{
    const pa_sample_spec sampleSpec = {PA_SAMPLE_S16LE, SAMPLE_RATE, 1};
    std::unique_ptr<PaSimpleStream> paStream(new PaSimpleStream(nullptr));

    int error;

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

    paStream.reset(new PaSimpleStream(pa_simple_new(NULL, argv[0], PA_STREAM_RECORD, NULL, "record", &sampleSpec, NULL, NULL, &error)));
    if (!paStream->GetStream())
    {
        std::cerr << "pa_simple_new error: " << pa_strerror(error) << std::endl;
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

    glDisable(GL_PROGRAM_POINT_SIZE);
    glPointSize(5.0f);

    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glLineWidth(0.5f);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    size_t numPoints = SAMPLE_RATE * 3;
    std::vector<glm::vec2> channelValues0;
    // std::vector<glm::vec2> channelValues1;
    channelValues0.reserve(numPoints);
    // channelValues1.reserve(numPoints);

    // vbo for first channel
    GLuint vbo0;
    glGenBuffers(1, &vbo0);
    if (vbo0 == 0)
    {
        std::cerr << "vbo created with id 0" << std::endl;
        return EXIT_FAILURE;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo0);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec2) * numPoints, nullptr, GL_DYNAMIC_DRAW);

    // vbo for second channel
    // GLuint vbo1;
    // glGenBuffers(1, &vbo1);
    // if (vbo1 == 0)
    // {
    //     std::cerr << "vbo created with id 0" << std::endl;
    //     return EXIT_FAILURE;
    // }
    // glBindBuffer(GL_ARRAY_BUFFER, vbo1);
    // glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec2) * numPoints, nullptr, GL_DYNAMIC_DRAW);

    GLuint vao0 = 0;
    glGenVertexArrays(1, &vao0);
    if (vao0 == 0)
    {
        std::cerr << "vao created with id 0" << std::endl;
        return EXIT_FAILURE;
    }
    // GLuint vao1 = 0;
    // glGenVertexArrays(1, &vao1);
    // if (vao1 == 0)
    // {
    //     std::cerr << "vao created with id 0" << std::endl;
    //     return EXIT_FAILURE;
    // }

    glBindVertexArray(vao0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo0);
    glVertexAttribPointer(glGetAttribLocation(program, "position"), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(glGetAttribLocation(program, "position"));

    // glBindVertexArray(vao1);
    // glBindBuffer(GL_ARRAY_BUFFER, vbo1);
    // glVertexAttribPointer(glGetAttribLocation(program, "position"), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    // glEnableVertexAttribArray(glGetAttribLocation(program, "position"));

    GLint colorUniform = glGetUniformLocation(program, "color");
    float xPosition = -1.0f;
    // float yPosition = 0.5f;
    size_t offset = 0;
    size_t count = 0;

    double fpsLimit = 1.0 / 60.0;
    double lastTime = glfwGetTime();
    double timer = lastTime;
    double secondsSinceReset = 0;

    int numFrames = 0;
    double deltaTime = 0;

    while (!glfwWindowShouldClose(window))
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update timer
        double currentTime = glfwGetTime();
        deltaTime += (currentTime - lastTime) / fpsLimit;
        lastTime = currentTime;

        uint8_t buf[BUFSIZE];
        
        if (pa_simple_read(paStream->GetStream(), buf, sizeof(buf), &error) < 0)
        {
            std::cerr << "pa_simple_read error: " << pa_strerror(error) << std::endl;
            return EXIT_FAILURE;
        }

        // Copy data to vertex buffers
        // each value should be 1/SAMPLE_RATE ahead?
        std::vector<glm::vec2> channelValuesPerFrame0;
        channelValuesPerFrame0.reserve(BUFSIZE / 2);
        for (int i = 0; i < BUFSIZE; i += 2)
        {
            PCM16 s1 = BytesToPcm16(buf[i + 1], buf[i]);
            //PCM16 s2 = BytesToPcm16(buf[i + 2], buf[i + 3]);
            float x = xPosition;
            float y = Pcm16ToFloat(s1) / 2.0f; // transform range from [-1,1] to [-0.5, 0.5]
            glm::vec2 pos = {x, y};
            channelValues0.push_back(pos);
            channelValuesPerFrame0.push_back(pos);
            count += 1;
            xPosition += 1.0f / SAMPLE_RATE;
        }

        glBindBuffer(GL_ARRAY_BUFFER, vbo0);
        glBufferSubData(GL_ARRAY_BUFFER, offset, sizeof(glm::vec2) * BUFSIZE / 2, channelValuesPerFrame0.data());
        offset += sizeof(glm::vec2) * BUFSIZE / 2;

        glUseProgram(program);

        glBindVertexArray(vao0);
        glUniform4f(colorUniform, 0.0f, 0.0f, 1.0f, 1.0f);
        glDrawArrays(GL_LINE_STRIP, 0, count);
        ++numFrames;

        if (glfwGetTime() - timer >= 1.0)
        {
            ++timer;
            ++secondsSinceReset;
            std::cout << "Fps: " << numFrames << std::endl;
            numFrames = 0;
        }
        if (channelValues0.size() >= numPoints)
        {
            xPosition = -1.0f;
            offset = 0;
            count = 0;
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // ensure clear
            glBindBuffer(GL_ARRAY_BUFFER, vbo0);
            glBufferSubData(GL_ARRAY_BUFFER, offset, numPoints, nullptr);
            channelValues0.clear();
            std::cout << "secondsSinceReset" << std::endl;
            secondsSinceReset = 0;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    return EXIT_SUCCESS;
}