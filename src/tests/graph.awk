#!/usr/bin/awk -f
function generate_plot_file(title, columns) {
    print "set term pdf",
        "\nset output \""title".pdf\"",
        "\nset style data histogram",
        "\nset style histogram cluster gap 1",
        "\nset style fill solid border -1",
        "\nset boxwidth 0.98",
        "\nset xtic rotate by -45 scale 0",
        "\nset title \""title" Resampling\"",
        "\nset ylabel \"Time (ms)\"",
        "\nset key above",
        "\nplot for [i=2:"columns+1"] \""title".txt\" using i:xticlabels(1) title columnheader" > title".plot"
}

function get_measurement_string(input_rate, output_rate, measurements) {
    out = sprintf ("%sk-%sk ", input_rate, output_rate)
    for (method in measurement)
        out = out sprintf ("%.0f ", measurement[method])
    return out
}

function get_header_string(format, measurement) {
    out = sprintf ("%s ", format)
    for (method in measurement)
        out = out sprintf ("%s ", method)
    return out
}

BEGIN {
    print_header = 1
}

/^Checking/ {
    if (flag == 1) {
        if (print_header) {
            out_file = format".txt"
            generate_plot_file(format, length(measurement))
            print get_header_string(format, measurement) >> format".txt"
            print_header = 0
        }

        print get_measurement_string(input_rate, output_rate, measurement) >> format".txt"
        flag = 0
    }

    if (format != $2) {
        delete measurement
        print_header = 1
    }

    format = $2
    input_rate = substr($4, 2) / 1000
    output_rate = substr($6, 0, length($6) - 1) / 1000

    flag=1; next
}

flag && $1 !~ /^[0-9]*%:/ {
    method = substr($1, 0, length($1) -1)
    time = $2 / 1000
    measurement[method] = time
}

END {
    print get_measurement_string(input_rate, output_rate, measurement) >> format".txt"
}
