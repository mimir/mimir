function formatDate(date, fmt) //A stolen function to display time in a nicer way
{ //TODO: Modify this to show localised time and place in a centralised js for the whole project to use
    function pad(value) 
    {
        return (value.toString().length < 2) ? '0' + value : value;
    }
    return fmt.replace(/%([a-zA-Z])/g, function (_, fmtCode) {
        switch (fmtCode) {
        case 'Y':
            return date.getUTCFullYear();
        case 'M':
            return pad(date.getUTCMonth() + 1);
        case 'd':
            return pad(date.getUTCDate());
        case 'H':
            return pad(date.getUTCHours());
        case 'm':
            return pad(date.getUTCMinutes());
        case 's':
            return pad(date.getUTCSeconds());
        default:
            throw new Error('Unsupported format code: ' + fmtCode);
        }
    });
}
//TODO: Create function and views to allow ajax GET of the JSON data
function initQuestionPage(question, answers, question_comments, answer_comments) 
{	
    //Puts the question content in the DOM
	$('#user_question').append('<h2>' + question.title + '</h3>');
    if (question.lesson != "null")
    {
        $('#user_question').append('<h4>Lesson: ' + question.lesson + '</h4>');
    }
    if (question.question != "null")
    {
        $('#user_question').append('<h4>Question: ' + question.question + '</h4>');
        $('#user_question').append('<h4>Answer: ' + question.answer + '</h4>');
    }
	$('#user_question').append('<p>' + question.content + '</p>');
	$('#user_question').append('<p>User: ' + question.user_tag + '</p>');
	$('#user_question').append('<p>Created: ' + formatDate(new Date(question.created), '%H:%m:%s %d/%M/%Y') + '</p>');
	$('#user_question').append('<p>Last Modified: ' + formatDate(new Date(question.modified), '%H:%m:%s %d/%M/%Y') + '</p>');
	
    //Adds comments to the question
	for(var i=0; i < question_comments.length; i++)
	{
		$('#user_question').append('<div id=question_comment' + i + ' class=user_comment>');
		$('#question_comment' + i).append('<p>' + question_comments[i].content + '</p>');
        $('#question_comment' + i).append('<p>User: ' + question_comments[i].user_tag + '</p>')
        $('#question_comment' + i).append('<p>Created: ' + formatDate(new Date(question_comments[i].created), '%H:%m:%s %d/%M/%Y') + '</p>')
        $('#question_comment' + i).append('<p>Last Modified: ' + formatDate(new Date(question_comments[i].modified), '%H:%m:%s %d/%M/%Y') + '</p>')
		$('#user_question').append('</div>');
	}
	
    //Adds the answers to the DOM
	for(var i=0; i < answers.length; i++)
	{
		$('#user_answers').append('<div id=answer' + i + ' class=user_answer>');
		$('#answer' + i).append('<p>' + answers[i].content + '</p>');
        $('#answer' + i).append('<p>User: ' + answers[i].user_tag + '</p>');
        $('#answer' + i).append('<p>Created: ' + formatDate(new Date(answers[i].created), '%H:%m:%s %d/%M/%Y') + '</p>');
        $('#answer' + i).append('<p>Modified: ' + formatDate(new Date(answers[i].modified), '%H:%m:%s %d/%M/%Y') + '</p>');
        
        //Adds the comments to the correct answers
        for(var j=0; j < answer_comments[i].length; j++)
        {
            $('#answer' + i).append('<div id=answer_comment' + i + ' class=user_comment>');
            $('#answer_comment' + i).append('<p>' + answer_comments[i][j].content + '</p>');
            $('#answer_comment' + i).append('<p>User: ' + answer_comments[i][j].user_tag + '</p>');
            $('#answer_comment' + i).append('<p>Created: ' + formatDate(new Date(answer_comments[i][j].created), '%H:%m:%s %d/%M/%Y') + '</p>');
            $('#answer_comment' + i).append('<p>Last Modified: ' + formatDate(new Date(answer_comments[i][j].modified), '%H:%m:%s %d/%M/%Y') + '</p>');
            $('#answer' + i).append('</div>');
        }
        
		$('#user_answers').append('</div>');
	}
    
}