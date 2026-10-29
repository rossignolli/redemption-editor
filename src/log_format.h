#pragma once

// The mod log is one std::ostream shared by every line. A line that sets
// std::fixed and setprecision(1) and does not restore them changes how every
// later line prints floats ("rate=1" for 1.019, "camera=7e+02" for 712.4).
// FormatScope keeps the stream's flags and precision as they were when it was
// made and gives them back when it goes out of scope; a line that wants its
// own float format makes one first.
//
// Pure: no engine access; unit tested in ReplayCoreTests.

#include <ios>
#include <ostream>

namespace replay::logfmt
{
    // The precision every line without a format of its own expects (the
    // stream's default).
    inline constexpr std::streamsize kDefaultPrecision = 6;

    class FormatScope
    {
    public:
        explicit FormatScope(std::ostream& stream)
            : m_stream(stream), m_flags(stream.flags()), m_precision(stream.precision())
        {
        }
        ~FormatScope()
        {
            m_stream.flags(m_flags);
            m_stream.precision(m_precision);
        }
        FormatScope(const FormatScope&) = delete;
        FormatScope& operator=(const FormatScope&) = delete;

    private:
        std::ostream& m_stream;
        std::ios_base::fmtflags m_flags;
        std::streamsize m_precision;
    };

    // The stream's default float format (general notation, 6 digits),
    // for a line that prints numbers whatever the lines before it left.
    inline std::ostream& DefaultFloat(std::ostream& stream)
    {
        stream.unsetf(std::ios_base::floatfield);
        stream.precision(kDefaultPrecision);
        return stream;
    }
}
