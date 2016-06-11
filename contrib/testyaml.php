#!/opt/local/bin/php56
<?php
	$input = stream_get_contents(STDIN);

	$data = yaml_parse($input);

	if($data !== FALSE) {
		print_r($data);
		echo "\n";
	}
	else {
		echo "invalid YAML document\n";
	}
?>
