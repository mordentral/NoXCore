-- Delving Mode's text filled in: a template's {fields} (a name of letters, digits and _ between
-- braces) each its value, as the C++'s fillText (script_data.cpp) does; a field not given is left
-- as it is.
local words = {}

function words.fill(template: string, fields: { [string]: string }): string
    return (string.gsub(template, "{([%w_]+)}", fields))
end

return words
